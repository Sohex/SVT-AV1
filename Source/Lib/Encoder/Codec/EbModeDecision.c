/*
* Copyright(c) 2019 Intel Corporation
* Copyright (c) 2016, Alliance for Open Media. All rights reserved
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

/***************************************
* Includes
***************************************/
#include <stdlib.h>
#include <limits.h>

#include "EbCommonUtils.h"
#include "EbSequenceControlSet.h"
#include "EbModeDecision.h"
#include "EbTransformUnit.h"
#include "EbModeDecisionProcess.h"
#include "EbMotionEstimation.h"

#include "av1me.h"
#include "hash.h"
#include "EbEncInterPrediction.h"
#include "EbRateDistortionCost.h"
#include "aom_dsp_rtcd.h"
#include "EbLog.h"
#include "EbResize.h"
#include "mcomp.h"
#define INCRMENT_CAND_TOTAL_COUNT(cnt)                                                     \
    MULTI_LINE_MACRO_BEGIN cnt++;                                                          \
    if (cnt >= MODE_DECISION_CANDIDATE_MAX_COUNT_Y)                                          \
        SVT_LOG(" ERROR: reaching limit for MODE_DECISION_CANDIDATE_MAX_COUNT %i\n", cnt); \
    MULTI_LINE_MACRO_END

int8_t av1_ref_frame_type(const MvReferenceFrame *const rf);
int    av1_filter_intra_allowed_bsize(uint8_t enable_filter_intra, BlockSize bs);

void av1_set_ref_frame(MvReferenceFrame *rf, int8_t ref_frame_type);

void check_mv_validity(int16_t x_mv, int16_t y_mv, uint8_t need_shift);
static INLINE int is_interintra_allowed_bsize(const BlockSize bsize) {
    return (bsize >= BLOCK_8X8) && (bsize <= BLOCK_32X32);
}

static INLINE int is_interintra_allowed_mode(const PredictionMode mode) {
    return (mode >= SINGLE_INTER_MODE_START) && (mode < SINGLE_INTER_MODE_END);
}

static INLINE int is_interintra_allowed_ref(const MvReferenceFrame rf[2]) {
    return (rf[0] > INTRA_FRAME) && (rf[1] <= INTRA_FRAME);
}
int svt_is_interintra_allowed(uint8_t enable_inter_intra, BlockSize sb_type, PredictionMode mode,
                              const MvReferenceFrame ref_frame[2]) {
    return enable_inter_intra && is_interintra_allowed_bsize((const BlockSize)sb_type) &&
           is_interintra_allowed_mode(mode) && is_interintra_allowed_ref(ref_frame);
}
//Given one reference frame identified by the pair (list_index,ref_index)
//indicate if ME data is valid
uint8_t is_me_data_present(
    struct ModeDecisionContext  *context_ptr,
    const MeSbResults           *me_results,
    uint8_t                      list_idx,
    uint8_t                      ref_idx){
    uint8_t total_me_cnt = me_results->total_me_candidate_index[context_ptr->me_block_offset];
    const MeCandidate *me_block_results = &me_results->me_candidate_array[context_ptr->me_cand_offset];
    for (uint32_t me_cand_i = 0; me_cand_i < total_me_cnt; ++me_cand_i){
        const MeCandidate *me_cand = &me_block_results[me_cand_i];
        assert(me_cand->direction <= 2);
        if (me_cand->direction == 0 || me_cand->direction == 2) {
            if (list_idx == me_cand->ref0_list && ref_idx == me_cand->ref_idx_l0)
                return 1;
        }
        if (me_cand->direction == 1 || me_cand->direction == 2) {
            if (list_idx == me_cand->ref1_list && ref_idx == me_cand->ref_idx_l1)
                return 1;
        }
    }
    return 0;
}
/********************************************
* Constants
********************************************/
// 1 - Regular uni-pred ,
// 2 - Regular uni-pred + Wedge compound Inter Intra
// 3 - Regular uni-pred + Wedge compound Inter Intra + Smooth compound Inter Intra

#define II_COUNT 3

static INLINE int is_inter_mode(PredictionMode mode) {
    return mode >= SINGLE_INTER_MODE_START && mode < SINGLE_INTER_MODE_END;
}
EbBool warped_motion_mode_allowed(PictureControlSet* pcs, ModeDecisionContext* ctx) {
    FrameHeader *frm_hdr = &pcs->parent_pcs_ptr->frm_hdr;
    return frm_hdr->allow_warped_motion && has_overlappable_candidates(ctx->blk_ptr) &&
        ctx->blk_geom->bwidth >= 8 && ctx->blk_geom->bheight >= 8 &&
        ctx->warped_motion_injection;
}
MotionMode obmc_motion_mode_allowed(const PictureControlSet *   pcs_ptr,
                                    struct ModeDecisionContext *context_ptr, const BlockSize bsize,
                                    MvReferenceFrame rf0, MvReferenceFrame rf1,
                                    PredictionMode mode) {
    // check if should cap the max block size for obmc
    if (context_ptr->obmc_ctrls.max_blk_size_16x16)
        if (block_size_wide[bsize] > 16 || block_size_high[bsize] > 16)
            return SIMPLE_TRANSLATION;
    if (!context_ptr->obmc_ctrls.enabled) return SIMPLE_TRANSLATION;
    FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;

    if (!frm_hdr->is_motion_mode_switchable) return SIMPLE_TRANSLATION;

    if (frm_hdr->force_integer_mv == 0) {
        const TransformationType gm_type = pcs_ptr->parent_pcs_ptr->global_motion[rf0].wmtype;
        if (is_global_mv_block(mode, bsize, gm_type)) return SIMPLE_TRANSLATION;
    }

    if (is_motion_variation_allowed_bsize(bsize) && is_inter_mode(mode) && rf1 != INTRA_FRAME &&
        !(rf1 > INTRA_FRAME)) // is_motion_variation_allowed_compound
    {
        if (!has_overlappable_candidates(context_ptr->blk_ptr)) // check_num_overlappable_neighbors
            return SIMPLE_TRANSLATION;

        return OBMC_CAUSAL;
    } else
        return SIMPLE_TRANSLATION;
}

void precompute_obmc_data(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr);

//static uint32_t  AntiContouringIntraMode[11] = { EB_INTRA_PLANAR, EB_INTRA_DC, EB_INTRA_HORIZONTAL, EB_INTRA_VERTICAL,
//EB_INTRA_MODE_2, EB_INTRA_MODE_6, EB_INTRA_MODE_14, EB_INTRA_MODE_18, EB_INTRA_MODE_22, EB_INTRA_MODE_30, EB_INTRA_MODE_34 };
int32_t have_newmv_in_inter_mode(PredictionMode mode) {
    return (mode == NEWMV || mode == NEW_NEWMV || mode == NEAREST_NEWMV || mode == NEW_NEARESTMV ||
            mode == NEAR_NEWMV || mode == NEW_NEARMV);
}
const uint32_t parent_index[85] = {
    0,  0,  0,  2,  2,  2,  2,  0,  7,  7,  7,  7,  0,  12, 12, 12, 12, 0,  17, 17, 17, 17,
    0,  0,  23, 23, 23, 23, 0,  28, 28, 28, 28, 0,  33, 33, 33, 33, 0,  38, 38, 38, 38, 0,
    0,  44, 44, 44, 44, 0,  49, 49, 49, 49, 0,  54, 54, 54, 54, 0,  59, 59, 59, 59, 0,  0,
    65, 65, 65, 65, 0,  70, 70, 70, 70, 0,  75, 75, 75, 75, 0,  80, 80, 80, 80};
/*
  NORMAL ORDER
  |-------------------------------------------------------------|
  | ref_idx          0            1           2            3       |
  | List0            LAST        LAST2        LAST3        GOLD    |
  | List1            BWD            ALT2        ALT                 |
  |-------------------------------------------------------------|
*/
#define INVALID_REF 0xF

#if CLN_GET_LIST_GET_REF
uint8_t ref_type_to_list_idx[REFS_PER_FRAME + 1] = { 0,0,0,0,0,1,1,1 };
uint8_t get_list_idx(uint8_t ref_type) {
    return ref_type_to_list_idx[ref_type];
}
uint8_t ref_type_to_ref_idx[REFS_PER_FRAME + 1] = { 0,0,1,2,3,0,1,2 };
uint8_t get_ref_frame_idx(uint8_t ref_type) {
    return ref_type_to_ref_idx[ref_type];
};
MvReferenceFrame to_ref_frame[2][4] = {
{ LAST_FRAME  , LAST2_FRAME  ,LAST3_FRAME , GOLDEN_FRAME },
{ BWDREF_FRAME, ALTREF2_FRAME,ALTREF_FRAME, INVALID_REF  } };

MvReferenceFrame svt_get_ref_frame_type(uint8_t list, uint8_t ref_idx) {
    return to_ref_frame[list][ref_idx];
};
#else
uint8_t get_list_idx(uint8_t ref_type) {
    if (ref_type == LAST_FRAME || ref_type == LAST2_FRAME || ref_type == LAST3_FRAME ||
        ref_type == GOLDEN_FRAME)
        return 0;
    else if (ref_type == BWDREF_FRAME || ref_type == ALTREF_FRAME || ref_type == ALTREF2_FRAME)
        return 1;
    else
        return (0);
};

uint8_t get_ref_frame_idx(uint8_t ref_type) {
    if (ref_type == LAST_FRAME || ref_type == BWDREF_FRAME)
        return 0;
    else if (ref_type == LAST2_FRAME || ref_type == ALTREF2_FRAME)
        return 1;
    else if (ref_type == LAST3_FRAME || ref_type == ALTREF_FRAME)
        return 2;
    else if (ref_type == GOLDEN_FRAME)
        return 3;
    else
        return (0);
};
MvReferenceFrame svt_get_ref_frame_type(uint8_t list, uint8_t ref_idx) {
    switch (list) {
    case 0:
        return (ref_idx == 0
                    ? LAST_FRAME
                    : ref_idx == 1
                          ? LAST2_FRAME
                          : ref_idx == 2 ? LAST3_FRAME : ref_idx == 3 ? GOLDEN_FRAME : INVALID_REF);
    case 1:
        return (ref_idx == 0
                    ? BWDREF_FRAME
                    : ref_idx == 1 ? ALTREF2_FRAME : ref_idx == 2 ? ALTREF_FRAME : INVALID_REF);
    default: return (INVALID_REF);
    }
};
#endif
extern uint32_t stage1ModesArray[];

uint8_t get_max_drl_index(uint8_t refmvCnt, PredictionMode mode);
int32_t svt_av1_mv_bit_cost(const MV *mv, const MV *ref, const int32_t *mvjcost, int32_t *mvcost[2],
                            int32_t weight);
#define MV_COST_WEIGHT 108
#define MAX_INTERINTRA_SB_SQUARE 32 * 32
EbErrorType intra_luma_prediction_for_interintra(ModeDecisionContext *md_context_ptr,
                                                 PictureControlSet *  pcs_ptr,
                                                 InterIntraMode       interintra_mode,
                                                 EbPictureBufferDesc *prediction_ptr);
#if CLN_FAST_COST
int64_t pick_wedge_fixed_sign(PictureControlSet *pcs_ptr,
#else
int64_t     pick_wedge_fixed_sign(ModeDecisionCandidate *candidate_ptr, PictureControlSet *pcs_ptr,
#endif
                                  ModeDecisionContext *context_ptr, const BlockSize bsize,
                                  const int16_t *const residual1, const int16_t *const diff10,
                                  const int8_t wedge_sign, int8_t *const best_wedge_index);
void        model_rd_for_sb_with_curvfit(PictureControlSet *  picture_control_set_ptr,
                                         ModeDecisionContext *context_ptr, BlockSize bsize, int bw, int bh,
                                         uint8_t *src_buf, uint32_t src_stride, uint8_t *pred_buf,
                                         uint32_t pred_stride, int plane_from, int plane_to, int mi_row,
                                         int mi_col, int *out_rate_sum, int64_t *out_dist_sum,
                                         int *plane_rate, int64_t *plane_sse, int64_t *plane_dist);

#if CLN_FAST_COST
static int64_t pick_interintra_wedge(
#else
static int64_t pick_interintra_wedge(ModeDecisionCandidate *candidate_ptr,
#endif
                                     PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                                     const BlockSize bsize, const uint8_t *const p0,
                                     const uint8_t *const p1, uint8_t *src_buf, uint32_t src_stride,
                                     int32_t *wedge_index_out) {
    assert(is_interintra_wedge_used(bsize));
    // assert(cpi->common.seq_params.enable_interintra_compound);

    const int bw = block_size_wide[bsize];
    const int bh = block_size_high[bsize];
    DECLARE_ALIGNED(32, int16_t, residual1[MAX_SB_SQUARE]); // src - pred1
    DECLARE_ALIGNED(32, int16_t, diff10[MAX_SB_SQUARE]); // pred1 - pred0
    if (context_ptr->hbd_mode_decision) {
        svt_aom_highbd_subtract_block(bh, bw, residual1, bw, src_buf, src_stride, p1, bw, EB_10BIT);
        svt_aom_highbd_subtract_block(bh, bw, diff10, bw, p1, bw, p0, bw, EB_10BIT);

    } else {
        svt_aom_subtract_block(bh, bw, residual1, bw, src_buf, src_stride, p1, bw);
        svt_aom_subtract_block(bh, bw, diff10, bw, p1, bw, p0, bw);
    }

    int8_t  wedge_index = -1;
    int64_t rd          = pick_wedge_fixed_sign(
#if CLN_FAST_COST
        pcs_ptr, context_ptr, bsize, residual1, diff10, 0, &wedge_index);
#else
        candidate_ptr, pcs_ptr, context_ptr, bsize, residual1, diff10, 0, &wedge_index);
#endif
    *wedge_index_out = wedge_index;

    return rd;
}
//for every CU, perform DC/V/H/S intra prediction to be used later in inter-intra search
void precompute_intra_pred_for_inter_intra(PictureControlSet *  pcs_ptr,
                                           ModeDecisionContext *context_ptr) {
    uint32_t            j;
    EbPictureBufferDesc pred_desc;
    pred_desc.origin_x = pred_desc.origin_y = 0;
    pred_desc.stride_y                      = context_ptr->blk_geom->bwidth;

    for (j = 0; j < INTERINTRA_MODES; ++j) {
        InterIntraMode interintra_mode = (InterIntraMode)j;
        pred_desc.buffer_y             = context_ptr->intrapred_buf[j];
        intra_luma_prediction_for_interintra(context_ptr, pcs_ptr, interintra_mode, &pred_desc);
    }
}

void combine_interintra(InterIntraMode mode, int8_t use_wedge_interintra, int wedge_index,
                        int wedge_sign, BlockSize bsize, BlockSize plane_bsize, uint8_t *comppred,
                        int compstride, const uint8_t *interpred, int interstride,
                        const uint8_t *intrapred, int intrastride);
void inter_intra_search(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                        ModeDecisionCandidate *candidate_ptr) {
#if FTR_SCALE_FACTOR
    SequenceControlSet *scs_ptr =
        (SequenceControlSet *)pcs_ptr->scs_wrapper_ptr->object_ptr;
#endif
    DECLARE_ALIGNED(16, uint8_t, tmp_buf[2 * MAX_INTERINTRA_SB_SQUARE]);
    DECLARE_ALIGNED(16, uint8_t, ii_pred_buf[2 * MAX_INTERINTRA_SB_SQUARE]);
    //get inter pred for ref0
    EbPictureBufferDesc *src_pic = context_ptr->hbd_mode_decision
                                       ? pcs_ptr->input_frame16bit
                                       : pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr;
    uint16_t *src_buf_hbd = (uint16_t *)src_pic->buffer_y +
                            (context_ptr->blk_origin_x + src_pic->origin_x) +
                            (context_ptr->blk_origin_y + src_pic->origin_y) * src_pic->stride_y;
    uint8_t *src_buf = src_pic->buffer_y + (context_ptr->blk_origin_x + src_pic->origin_x) +
                       (context_ptr->blk_origin_y + src_pic->origin_y) * src_pic->stride_y;

    uint8_t bit_depth = context_ptr->hbd_mode_decision ? EB_10BIT : EB_8BIT;
    uint32_t full_lambda =  context_ptr->hbd_mode_decision ?
        context_ptr->full_lambda_md[EB_10_BIT_MD] :
        context_ptr->full_lambda_md[EB_8_BIT_MD];

    uint32_t            bwidth  = context_ptr->blk_geom->bwidth;
    uint32_t            bheight = context_ptr->blk_geom->bheight;
    EbPictureBufferDesc pred_desc;
    pred_desc.origin_x = pred_desc.origin_y = 0;
    pred_desc.stride_y                      = bwidth;

    EbPictureBufferDesc *ref_pic_list0;
    EbPictureBufferDesc *ref_pic_list1 = NULL;
    Mv                   mv_0;
    Mv                   mv_1;
    mv_0.x = candidate_ptr->motion_vector_xl0;
    mv_0.y = candidate_ptr->motion_vector_yl0;
    mv_1.x = candidate_ptr->motion_vector_xl1;
    mv_1.y = candidate_ptr->motion_vector_yl1;
    MvUnit mv_unit;
    mv_unit.mv[0]               = mv_0;
    mv_unit.mv[1]               = mv_1;
#if !CLN_MD_CANDS
    int8_t           ref_idx_l0 = candidate_ptr->ref_frame_index_l0;
    int8_t           ref_idx_l1 = candidate_ptr->ref_frame_index_l1;
#endif
    MvReferenceFrame rf[2];
    av1_set_ref_frame(rf, candidate_ptr->ref_frame_type);
#if CLN_MD_CANDS
    int8_t ref_idx_l0 = get_ref_frame_idx(rf[0]);
    int8_t ref_idx_l1 = rf[1] == NONE_FRAME ? get_ref_frame_idx(rf[0]) : get_ref_frame_idx(rf[1]);
#endif
    uint8_t list_idx0, list_idx1;
    list_idx0 = get_list_idx(rf[0]);
    if (rf[1] == NONE_FRAME)
        list_idx1 = get_list_idx(rf[0]);
    else
        list_idx1 = get_list_idx(rf[1]);
    assert(list_idx0 < MAX_NUM_OF_REF_PIC_LIST);
    assert(list_idx1 < MAX_NUM_OF_REF_PIC_LIST);
    //
    if (ref_idx_l0 >= 0)
        ref_pic_list0 =
            context_ptr->hbd_mode_decision
                ? ((EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[list_idx0][ref_idx_l0]
                       ->object_ptr)
                      ->reference_picture16bit
                : ((EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[list_idx0][ref_idx_l0]
                       ->object_ptr)
                      ->reference_picture;
    else
        ref_pic_list0 = (EbPictureBufferDesc *)NULL;

    if (ref_idx_l1 >= 0)
        ref_pic_list1 =
            context_ptr->hbd_mode_decision
                ? ((EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[list_idx1][ref_idx_l1]
                       ->object_ptr)
                      ->reference_picture16bit
                : ((EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[list_idx1][ref_idx_l1]
                       ->object_ptr)
                      ->reference_picture;
    else
        ref_pic_list1 = (EbPictureBufferDesc *)NULL;

    // Use scaled references if resolution of the reference is different from that of the input
    if(ref_pic_list0 != NULL)
        use_scaled_rec_refs_if_needed(pcs_ptr,
                                      pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr,
                                      (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[list_idx0][list_idx0]->object_ptr,
                                      &ref_pic_list0);
    if(ref_pic_list1 != NULL)
        use_scaled_rec_refs_if_needed(pcs_ptr,
                                      pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr,
                                      (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[list_idx1][ref_idx_l1]->object_ptr,
                                      &ref_pic_list1);

    mv_unit.pred_direction = candidate_ptr->prediction_direction[0];

    pred_desc.buffer_y = tmp_buf;

    //we call the regular inter prediction path here(no compound)
    av1_inter_prediction(
#if FTR_SCALE_FACTOR
        scs_ptr,
#endif
        pcs_ptr,
        0, //ASSUMPTION: fixed interpolation filter.
        context_ptr->blk_ptr,
        candidate_ptr->ref_frame_type,
        &mv_unit,
        0, //use_intrabc,
        SIMPLE_TRANSLATION,
        0,
        0,
        1, //compound_idx not used
        NULL, // interinter_comp not used
#if !OPT_INIT_XD_2
        NULL,
#endif
        NULL,
        NULL,
        NULL,
        0,
        0,
        0,
        0,
        context_ptr->blk_origin_x,
        context_ptr->blk_origin_y,
        bwidth,
        bheight,
        ref_pic_list0,
        ref_pic_list1,
        &pred_desc, //output
        0, //output origin_x,
        0, //output origin_y,
        0, //do chroma
        context_ptr->hbd_mode_decision ? EB_10BIT : EB_8BIT);

    assert(is_interintra_wedge_used(
        context_ptr->blk_geom->bsize)); //if not I need to add nowedge path!!

    int64_t        best_interintra_rd = INT64_MAX;
    int            rate_sum;
    int64_t        dist_sum;
    int            tmp_rate_mv              = 0;
    InterIntraMode best_interintra_mode     = INTERINTRA_MODES;
    for (int j = 0; j < INTERINTRA_MODES; ++j) {
        //if ((!cpi->oxcf.enable_smooth_intra || cpi->sf.disable_smooth_intra) &&
        //    (InterIntraMode)j == II_SMOOTH_PRED)
        //  continue;
        InterIntraMode interintra_mode = (InterIntraMode)j;
        //rmode = interintra_mode_cost[mbmi->interintra_mode];
        const int bsize_group = size_group_lookup[context_ptr->blk_geom->bsize];
#if CLN_FAST_COST
        const int rmode = context_ptr->md_rate_estimation_ptr
            ->inter_intra_mode_fac_bits[bsize_group][interintra_mode];
#else
        const int rmode       = candidate_ptr->md_rate_estimation_ptr
                              ->inter_intra_mode_fac_bits[bsize_group][interintra_mode];
#endif
        //av1_combine_interintra(xd, bsize, 0, tmp_buf, bw, intrapred, bw);
        if (context_ptr->hbd_mode_decision)
            combine_interintra_highbd(interintra_mode, //mode,
                                      0, //use_wedge_interintra,
                                      0, //candidate_ptr->interintra_wedge_index,
                                      0, //int wedge_sign,
                                      context_ptr->blk_geom->bsize,
                                      context_ptr->blk_geom->bsize, // plane_bsize,
                                      ii_pred_buf,
                                      bwidth, /*uint8_t *comppred, int compstride,*/
                                      tmp_buf,
                                      bwidth, /*const uint8_t *interpred, int interstride,*/
                                      context_ptr->intrapred_buf[j],
                                      bwidth /*const uint8_t *intrapred,   int intrastride*/,
                                      bit_depth);
        else

            combine_interintra(interintra_mode, //mode,
                               0, //use_wedge_interintra,
                               0, //candidate_ptr->interintra_wedge_index,
                               0, //int wedge_sign,
                               context_ptr->blk_geom->bsize,
                               context_ptr->blk_geom->bsize, // plane_bsize,
                               ii_pred_buf,
                               bwidth, /*uint8_t *comppred, int compstride,*/
                               tmp_buf,
                               bwidth, /*const uint8_t *interpred, int interstride,*/
                               context_ptr->intrapred_buf[j],
                               bwidth /*const uint8_t *intrapred,   int intrastride*/);

        //model_rd_sb_fn[MODELRD_TYPE_INTERINTRA](
        //    cpi, bsize, x, xd, 0, 0, mi_row, mi_col, &rate_sum, &dist_sum,
        //    &tmp_skip_txfm_sb, &tmp_skip_sse_sb, NULL, NULL, NULL);
        model_rd_for_sb_with_curvfit(
            pcs_ptr,
            context_ptr,
            context_ptr->blk_geom->bsize,
            bwidth,
            bheight,
            context_ptr->hbd_mode_decision ? (uint8_t *)src_buf_hbd : src_buf,
            src_pic->stride_y,
            ii_pred_buf,
            bwidth,
            0,
            0,
            0,
            0,
            &rate_sum,
            &dist_sum,
            NULL,
            NULL,
            NULL);
        // rd = RDCOST(x->rdmult, tmp_rate_mv + rate_sum + rmode, dist_sum);
        int64_t rd = RDCOST(full_lambda, tmp_rate_mv + rate_sum + rmode, dist_sum);

        if (rd < best_interintra_rd) {
            best_interintra_rd             = rd;
            candidate_ptr->interintra_mode = best_interintra_mode = interintra_mode;
        }
    }

    /* best_interintra_rd_wedge =
            pick_interintra_wedge(cpi, x, bsize, intrapred_, tmp_buf_);*/

    //CHKN need to re-do intra pred using the winner, or have a separate intra serch for wedge
#if CLN_FAST_COST
    pick_interintra_wedge(
#else
    pick_interintra_wedge(candidate_ptr,
#endif
                          pcs_ptr,
                          context_ptr,
                          context_ptr->blk_geom->bsize,
                          context_ptr->intrapred_buf[best_interintra_mode],
                          tmp_buf,
                          context_ptr->hbd_mode_decision ? (uint8_t *)src_buf_hbd : src_buf,
                          src_pic->stride_y,
                          &candidate_ptr->interintra_wedge_index);

    //if (best_interintra_rd_wedge < best_interintra_rd) {

    //candidate_ptr->use_wedge_interintra = 1;
    //candidate_ptr->ii_wedge_sign = 0;
    //}
    //args->inter_intra_mode[mbmi->ref_frame[0]] = best_interintra_mode;
    // Enable wedge search if source variance and edge strength are above the thresholds.
}

COMPOUND_TYPE to_av1_compound_lut[] = {
    COMPOUND_AVERAGE, COMPOUND_DISTWTD, COMPOUND_DIFFWTD, COMPOUND_WEDGE};

void determine_compound_mode(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                             ModeDecisionCandidate *candidatePtr, MD_COMP_TYPE cur_type) {
    candidatePtr->interinter_comp.type = to_av1_compound_lut[cur_type];

    if (cur_type == MD_COMP_AVG) {
        candidatePtr->comp_group_idx = 0;
        candidatePtr->compound_idx   = 1;
    } else if (cur_type == MD_COMP_DIST) {
        candidatePtr->comp_group_idx = 0;
        candidatePtr->compound_idx   = 0;
    } else if (cur_type == MD_COMP_DIFF0) {
        candidatePtr->comp_group_idx            = 1;
        candidatePtr->compound_idx              = 1;
        candidatePtr->interinter_comp.mask_type = 55;
        search_compound_diff_wedge(pcs_ptr, context_ptr, candidatePtr);

    }
    //else if (cur_type == MD_COMP_DIFF1) {
    //    candidatePtr->comp_group_idx = 1;
    //    candidatePtr->compound_idx = 1;
    //    candidatePtr->interinter_comp.mask_type = 1;
    //}
    else if (cur_type == MD_COMP_WEDGE) {
        candidatePtr->comp_group_idx = 1;
        candidatePtr->compound_idx   = 1;
        search_compound_diff_wedge(pcs_ptr, context_ptr, candidatePtr);
    } else {
        SVT_LOG("ERROR: not used comp type\n");
    }
}

void choose_best_av1_mv_pred(ModeDecisionContext *           context_ptr,
                             struct MdRateEstimationContext *md_rate_estimation_ptr,
                             BlkStruct *blk_ptr, MvReferenceFrame ref_frame, uint8_t is_compound,
                             PredictionMode mode, //NEW or NEW_NEW
                             int16_t mv0x, int16_t mv0y, int16_t mv1x, int16_t mv1y,
                             uint8_t *bestDrlIndex, // output
                             IntMv    best_pred_mv[2] // output
) {
    if (context_ptr->shut_fast_rate) {
        return;
    }
    uint8_t  drli, max_drl_index;
    IntMv    nearestmv[2] = {{0}};
    IntMv    nearmv[2];
    IntMv    ref_mv[2];
    uint32_t best_mv_cost = 0xFFFFFFFF;
    MV       mv;

    max_drl_index = get_max_drl_index(blk_ptr->av1xd->ref_mv_count[ref_frame], mode);
    // max_drl_index = 1;

    for (drli = 0; drli < max_drl_index; drli++) {
        get_av1_mv_pred_drl(
            context_ptr, blk_ptr, ref_frame, is_compound, mode, drli, nearestmv, nearmv, ref_mv);

        //compute the rate for this drli Cand
        mv.row = mv0y;
        mv.col = mv0x;

        uint32_t mv_rate = (uint32_t)svt_av1_mv_bit_cost(&mv,
                                                         &(ref_mv[0].as_mv),
                                                         md_rate_estimation_ptr->nmv_vec_cost,
                                                         md_rate_estimation_ptr->nmvcoststack,
                                                         MV_COST_WEIGHT);

        if (is_compound) {
            mv.row = mv1y;
            mv.col = mv1x;

            mv_rate += (uint32_t)svt_av1_mv_bit_cost(&mv,
                                                     &(ref_mv[1].as_mv),
                                                     md_rate_estimation_ptr->nmv_vec_cost,
                                                     md_rate_estimation_ptr->nmvcoststack,
                                                     MV_COST_WEIGHT);
        }

        if (mv_rate < best_mv_cost) {
            best_mv_cost    = mv_rate;
            *bestDrlIndex   = drli;
            best_pred_mv[0] = ref_mv[0];
            best_pred_mv[1] = ref_mv[1];
        }
    }
}

static void mode_decision_candidate_buffer_dctor(EbPtr p) {
    ModeDecisionCandidateBuffer *obj = (ModeDecisionCandidateBuffer *)p;
    EB_DELETE(obj->prediction_ptr);
    EB_DELETE(obj->recon_coeff_ptr);
}
static void mode_decision_scratch_candidate_buffer_dctor(EbPtr p) {
    ModeDecisionCandidateBuffer *obj = (ModeDecisionCandidateBuffer *)p;
    EB_DELETE(obj->prediction_ptr);
    EB_DELETE(obj->residual_ptr);
    EB_DELETE(obj->recon_coeff_ptr);
    EB_DELETE(obj->recon_ptr);
}
/***************************************
* Mode Decision Candidate Ctor
***************************************/
EbErrorType mode_decision_candidate_buffer_ctor(ModeDecisionCandidateBuffer *buffer_ptr,
                                                EbBitDepthEnum               max_bitdepth,
                                                uint8_t sb_size,
                                                uint32_t buffer_desc_mask,
                                                EbPictureBufferDesc *temp_residual_ptr,
                                                EbPictureBufferDesc *temp_recon_ptr,
                                                uint64_t *fast_cost_ptr, uint64_t *full_cost_ptr,
                                                uint64_t *full_cost_skip_ptr,
                                                uint64_t *full_cost_merge_ptr) {
    EbPictureBufferDescInitData picture_buffer_desc_init_data;

    EbPictureBufferDescInitData thirty_two_width_picture_buffer_desc_init_data;

    buffer_ptr->dctor = mode_decision_candidate_buffer_dctor;

    // Init Picture Data
    picture_buffer_desc_init_data.max_width                       = sb_size;
    picture_buffer_desc_init_data.max_height                      = sb_size;
    picture_buffer_desc_init_data.bit_depth                       = max_bitdepth;
    picture_buffer_desc_init_data.color_format                    = EB_YUV420;
    picture_buffer_desc_init_data.buffer_enable_mask              = buffer_desc_mask;
    picture_buffer_desc_init_data.left_padding                    = 0;
    picture_buffer_desc_init_data.right_padding                   = 0;
    picture_buffer_desc_init_data.top_padding                     = 0;
    picture_buffer_desc_init_data.bot_padding                     = 0;
    picture_buffer_desc_init_data.split_mode                      = EB_FALSE;

    thirty_two_width_picture_buffer_desc_init_data.max_width    = sb_size;
    thirty_two_width_picture_buffer_desc_init_data.max_height   = sb_size;
    thirty_two_width_picture_buffer_desc_init_data.bit_depth    = EB_32BIT;
    thirty_two_width_picture_buffer_desc_init_data.color_format = EB_YUV420;
    thirty_two_width_picture_buffer_desc_init_data.buffer_enable_mask =
        buffer_desc_mask;
    thirty_two_width_picture_buffer_desc_init_data.left_padding  = 0;
    thirty_two_width_picture_buffer_desc_init_data.right_padding = 0;
    thirty_two_width_picture_buffer_desc_init_data.top_padding   = 0;
    thirty_two_width_picture_buffer_desc_init_data.bot_padding   = 0;
    thirty_two_width_picture_buffer_desc_init_data.split_mode    = EB_FALSE;

    // Candidate Ptr
    buffer_ptr->candidate_ptr = (ModeDecisionCandidate *)NULL;

    // Video Buffers
    EB_NEW(buffer_ptr->prediction_ptr,
           svt_picture_buffer_desc_ctor,
           (EbPtr)&picture_buffer_desc_init_data);
    // Reuse the residual_ptr memory in MD context
    buffer_ptr->residual_ptr = temp_residual_ptr;
    EB_NEW(buffer_ptr->recon_coeff_ptr,
           svt_picture_buffer_desc_ctor,
           (EbPtr)&thirty_two_width_picture_buffer_desc_init_data);
    // Reuse the recon_ptr memory in MD context
    buffer_ptr->recon_ptr = temp_recon_ptr;

    // Costs
    buffer_ptr->fast_cost_ptr       = fast_cost_ptr;
    buffer_ptr->full_cost_ptr       = full_cost_ptr;
    buffer_ptr->full_cost_skip_ptr  = full_cost_skip_ptr;
    buffer_ptr->full_cost_merge_ptr = full_cost_merge_ptr;
    return EB_ErrorNone;
}
EbErrorType mode_decision_scratch_candidate_buffer_ctor(ModeDecisionCandidateBuffer *buffer_ptr,
                                                        uint8_t                      sb_size,
                                                        EbBitDepthEnum               max_bitdepth) {
    EbPictureBufferDescInitData picture_buffer_desc_init_data;
    EbPictureBufferDescInitData double_width_picture_buffer_desc_init_data;
    EbPictureBufferDescInitData thirty_two_width_picture_buffer_desc_init_data;

    buffer_ptr->dctor = mode_decision_scratch_candidate_buffer_dctor;

    // Init Picture Data
    picture_buffer_desc_init_data.max_width                       = sb_size;
    picture_buffer_desc_init_data.max_height                      = sb_size;
    picture_buffer_desc_init_data.bit_depth                       = max_bitdepth;
    picture_buffer_desc_init_data.color_format                    = EB_YUV420;
    picture_buffer_desc_init_data.buffer_enable_mask              = PICTURE_BUFFER_DESC_FULL_MASK;
    picture_buffer_desc_init_data.left_padding                    = 0;
    picture_buffer_desc_init_data.right_padding                   = 0;
    picture_buffer_desc_init_data.top_padding                     = 0;
    picture_buffer_desc_init_data.bot_padding                     = 0;
    picture_buffer_desc_init_data.split_mode                      = EB_FALSE;
    double_width_picture_buffer_desc_init_data.max_width          = sb_size;
    double_width_picture_buffer_desc_init_data.max_height         = sb_size;
    double_width_picture_buffer_desc_init_data.bit_depth          = EB_16BIT;
    double_width_picture_buffer_desc_init_data.color_format       = EB_YUV420;
    double_width_picture_buffer_desc_init_data.buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK;
    double_width_picture_buffer_desc_init_data.left_padding       = 0;
    double_width_picture_buffer_desc_init_data.right_padding      = 0;
    double_width_picture_buffer_desc_init_data.top_padding        = 0;
    double_width_picture_buffer_desc_init_data.bot_padding        = 0;
    double_width_picture_buffer_desc_init_data.split_mode         = EB_FALSE;
    thirty_two_width_picture_buffer_desc_init_data.max_width    = sb_size;
    thirty_two_width_picture_buffer_desc_init_data.max_height   = sb_size;
    thirty_two_width_picture_buffer_desc_init_data.bit_depth    = EB_32BIT;
    thirty_two_width_picture_buffer_desc_init_data.color_format = EB_YUV420;
    thirty_two_width_picture_buffer_desc_init_data.buffer_enable_mask =
        PICTURE_BUFFER_DESC_FULL_MASK;
    thirty_two_width_picture_buffer_desc_init_data.left_padding  = 0;
    thirty_two_width_picture_buffer_desc_init_data.right_padding = 0;
    thirty_two_width_picture_buffer_desc_init_data.top_padding   = 0;
    thirty_two_width_picture_buffer_desc_init_data.bot_padding   = 0;
    thirty_two_width_picture_buffer_desc_init_data.split_mode    = EB_FALSE;

    // Candidate Ptr
    buffer_ptr->candidate_ptr = (ModeDecisionCandidate *)NULL;

    // Video Buffers
    EB_NEW(buffer_ptr->prediction_ptr,
           svt_picture_buffer_desc_ctor,
           (EbPtr)&picture_buffer_desc_init_data);
    EB_NEW(buffer_ptr->residual_ptr,
           svt_picture_buffer_desc_ctor,
           (EbPtr)&double_width_picture_buffer_desc_init_data);
    EB_NEW(buffer_ptr->recon_coeff_ptr,
           svt_picture_buffer_desc_ctor,
           (EbPtr)&thirty_two_width_picture_buffer_desc_init_data);

    EB_NEW(
        buffer_ptr->recon_ptr, svt_picture_buffer_desc_ctor, (EbPtr)&picture_buffer_desc_init_data);
    return EB_ErrorNone;
}
/***************************************
* return true if the MV candidate is already injected
***************************************/
EbBool mrp_is_already_injected_mv_l0(ModeDecisionContext *context_ptr, int16_t mv_x, int16_t mv_y,
                                     uint8_t ref_type) {
    for (int inter_candidate_index = 0; inter_candidate_index < context_ptr->injected_mv_count_l0;
         inter_candidate_index++) {
        if (context_ptr->injected_mv_x_l0_array[inter_candidate_index] == mv_x &&
            context_ptr->injected_mv_y_l0_array[inter_candidate_index] == mv_y &&
            context_ptr->injected_ref_type_l0_array[inter_candidate_index] == ref_type) {
            return (EB_TRUE);
        }
    }

    return (EB_FALSE);
}

EbBool mrp_is_already_injected_mv_l1(ModeDecisionContext *context_ptr, int16_t mv_x, int16_t mv_y,
                                     uint8_t ref_type) {
    for (int inter_candidate_index = 0; inter_candidate_index < context_ptr->injected_mv_count_l1;
         inter_candidate_index++) {
        if (context_ptr->injected_mv_x_l1_array[inter_candidate_index] == mv_x &&
            context_ptr->injected_mv_y_l1_array[inter_candidate_index] == mv_y &&
            context_ptr->injected_ref_type_l1_array[inter_candidate_index] == ref_type) {
            return (EB_TRUE);
        }
    }

    return (EB_FALSE);
}

EbBool mrp_is_already_injected_mv_bipred(ModeDecisionContext *context_ptr, int16_t mv_x_l0,
                                         int16_t mv_y_l0, int16_t mv_x_l1, int16_t mv_y_l1,
                                         uint8_t ref_type) {
    for (int inter_candidate_index = 0;
         inter_candidate_index < context_ptr->injected_mv_count_bipred;
         inter_candidate_index++) {
        if (context_ptr->injected_mv_x_bipred_l0_array[inter_candidate_index] == mv_x_l0 &&
            context_ptr->injected_mv_y_bipred_l0_array[inter_candidate_index] == mv_y_l0 &&
            context_ptr->injected_mv_x_bipred_l1_array[inter_candidate_index] == mv_x_l1 &&
            context_ptr->injected_mv_y_bipred_l1_array[inter_candidate_index] == mv_y_l1 &&
            context_ptr->injected_ref_type_bipred_array[inter_candidate_index] == ref_type) {
            return (EB_TRUE);
        }
    }
    return (EB_FALSE);
}

EbBool is_valid_unipred_ref(
    struct ModeDecisionContext *context_ptr,
    uint8_t inter_cand_group,
    uint8_t list_idx, uint8_t ref_idx) {
    if (!context_ptr->ref_pruning_ctrls.enabled)
        return EB_TRUE;
    if (!context_ptr->ref_filtering_res[inter_cand_group][list_idx][ref_idx].do_ref && (ref_idx || !context_ptr->ref_pruning_ctrls.closest_refs[inter_cand_group])) {
        return EB_FALSE;
    }
    else {
        return EB_TRUE;
    }
}

#if FTR_NEW_REF_PRUNING_CTRLS
// Determine if a unipred reference is valid, based on the current
// prediction type (i.e. inter_cand_group)
EbBool is_valid_uni_type(
    struct ModeDecisionContext *context_ptr,
    uint8_t inter_type,
    uint8_t is_ii_allowed, uint8_t is_warp_allowed,
    uint8_t list_idx, uint8_t ref_idx) {

    uint8_t inter_cand_group = TOT_INTER_GROUP;

    switch (inter_type) {
    case 0: // default
        return EB_TRUE;
        break;
    case 1:
    case 2:
        inter_cand_group = is_ii_allowed
            ? INTER_INTRA_GROUP
            : is_warp_allowed
            ? WARP_GROUP
            : OBMC_GROUP;

        return is_valid_unipred_ref(
            context_ptr, MIN(TOT_INTER_GROUP - 1, inter_cand_group), list_idx, ref_idx);
        break;
    case 3:// warp
        inter_cand_group = is_warp_allowed
            ? WARP_GROUP
            : OBMC_GROUP;
        return is_valid_unipred_ref(
            context_ptr, MIN(TOT_INTER_GROUP - 1, inter_cand_group), list_idx, ref_idx);
        break;
    case 4:// obmc
        inter_cand_group = OBMC_GROUP;
        return is_valid_unipred_ref(
            context_ptr, MIN(TOT_INTER_GROUP - 1, inter_cand_group), list_idx, ref_idx);
        break;
    default:
        assert(0);
        return EB_FALSE;
        break;
    }
}
#endif

EbBool is_valid_bipred_ref(
    struct ModeDecisionContext *context_ptr,
    uint8_t inter_cand_group,
    uint8_t list_idx_0, uint8_t ref_idx_0,
    uint8_t list_idx_1, uint8_t ref_idx_1) {
    if (!context_ptr->ref_pruning_ctrls.enabled)
        return EB_TRUE;
    // Both ref should be 1 for bipred refs to be valid: if 1 is not best_refs then there is a chance to exit the injection
    if (!context_ptr->ref_filtering_res[inter_cand_group][list_idx_0][ref_idx_0].do_ref ||
        !context_ptr->ref_filtering_res[inter_cand_group][list_idx_1][ref_idx_1].do_ref )
    {
        // Check whether we should check the closest, if no then there no need to move forward and return false
        if (!context_ptr->ref_pruning_ctrls.closest_refs[inter_cand_group])
            return EB_FALSE;

        // Else check if ref are LAST and BWD, if not then return false
        if (ref_idx_0 || ref_idx_1)
            return EB_FALSE;
    }
    return EB_TRUE;
}
#if FTR_NEW_REF_PRUNING_CTRLS
// Determine if a bipred reference is valid, based on the current
// prediction type (i.e. inter_cand_group)
EbBool is_valid_bi_type(
    struct ModeDecisionContext *context_ptr,
    MD_COMP_TYPE cur_type,
    uint8_t list_idx_0, uint8_t ref_idx_0,
    uint8_t list_idx_1, uint8_t ref_idx_1) {
    switch (cur_type) {
    case MD_COMP_AVG:
        return EB_TRUE;
        break;
    case MD_COMP_DIST:
        return is_valid_bipred_ref(
            context_ptr, COMP_DIST, list_idx_0, ref_idx_0, list_idx_1, ref_idx_1);
        break;
    case MD_COMP_DIFF0:
        return is_valid_bipred_ref(
            context_ptr, COMP_DIFF, list_idx_0, ref_idx_0, list_idx_1, ref_idx_1);
        break;
    case MD_COMP_WEDGE:
        return is_valid_bipred_ref(
            context_ptr, COMP_WEDGE, list_idx_0, ref_idx_0, list_idx_1, ref_idx_1);
        break;
    default:
        assert(0);
        return EB_FALSE;
        break;
    }
}
#endif
#define BIPRED_3x3_REFINMENT_POSITIONS 8

int8_t allow_refinement_flag[BIPRED_3x3_REFINMENT_POSITIONS] = {1, 0, 1, 0, 1, 0, 1, 0};
int8_t bipred_3x3_x_pos[BIPRED_3x3_REFINMENT_POSITIONS]      = {-1, -1, 0, 1, 1, 1, 0, -1};
int8_t bipred_3x3_y_pos[BIPRED_3x3_REFINMENT_POSITIONS]      = {0, 1, 1, 1, 0, -1, -1, -1};

void unipred_3x3_candidates_injection(const SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr,
                                      ModeDecisionContext *context_ptr, SuperBlock *sb_ptr,
                                      uint32_t me_sb_addr, uint32_t *candidate_total_cnt) {
    UNUSED(sb_ptr);
    uint32_t           bipred_index;
    uint32_t           cand_total_cnt = (*candidate_total_cnt);
    FrameHeader *      frm_hdr        = &pcs_ptr->parent_pcs_ptr->frm_hdr;
    MeSbResults *me_results = pcs_ptr->parent_pcs_ptr->pa_me_data->me_results[me_sb_addr];
    uint8_t total_me_cnt = me_results->total_me_candidate_index[context_ptr->me_block_offset];
    const MeCandidate *me_block_results = &me_results->me_candidate_array[context_ptr->me_cand_offset];
    ModeDecisionCandidate *cand_array   = context_ptr->fast_candidate_array;
    EbBool       is_compound_enabled    = (frm_hdr->reference_mode == SINGLE_REFERENCE) ? 0 : 1;
    IntMv        best_pred_mv[2]        = {{0}, {0}};
    int          inside_tile            = 1;
    MacroBlockD *xd                     = context_ptr->blk_ptr->av1xd;
    int          umv0tile               = (scs_ptr->static_config.unrestricted_motion_vector == 0);
    uint32_t     mi_row                 = context_ptr->blk_origin_y >> MI_SIZE_LOG2;
    uint32_t     mi_col                 = context_ptr->blk_origin_x >> MI_SIZE_LOG2;

    // (8 Best_L0 neighbors)
    for (uint8_t me_candidate_index = 0; me_candidate_index < total_me_cnt; ++me_candidate_index) {
        const MeCandidate *me_block_results_ptr = &me_block_results[me_candidate_index];
        const uint8_t      inter_direction      = me_block_results_ptr->direction;
        const uint8_t      list0_ref_index      = me_block_results_ptr->ref_idx_l0;
        if (inter_direction == 0) {
        if (!is_valid_unipred_ref(context_ptr, MIN(TOT_INTER_GROUP-1,UNI_3x3_GROUP), REF_LIST_0, list0_ref_index)) continue;
#if !FTR_NEW_REF_PRUNING_CTRLS
            if (list0_ref_index > context_ptr->md_max_ref_count - 1)
                continue;
#endif
            for (bipred_index = 0; bipred_index < BIPRED_3x3_REFINMENT_POSITIONS; ++bipred_index) {
                /**************
        NEWMV L0
        ************* */
                if (context_ptr->unipred3x3_injection >= 2) {
                    if (allow_refinement_flag[bipred_index] == 0) continue;
                }
                int16_t to_inject_mv_x;
                int16_t to_inject_mv_y;
                if (pcs_ptr->parent_pcs_ptr->frm_hdr.allow_high_precision_mv) {
                    to_inject_mv_x = context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                        [REF_LIST_0][list0_ref_index][0] +
                        bipred_3x3_x_pos[bipred_index];
                    to_inject_mv_y = context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                        [REF_LIST_0][list0_ref_index][1] +
                        bipred_3x3_y_pos[bipred_index];
                }
                else {
                    to_inject_mv_x = context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                        [REF_LIST_0][list0_ref_index][0] +
                        (bipred_3x3_x_pos[bipred_index] << 1);
                    to_inject_mv_y = context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                        [REF_LIST_0][list0_ref_index][1] +
                        (bipred_3x3_y_pos[bipred_index] << 1);
                }
                uint8_t to_inject_ref_type = svt_get_ref_frame_type(REF_LIST_0, list0_ref_index);

                inside_tile = 1;
                if (umv0tile)
                    inside_tile = is_inside_tile_boundary(&(xd->tile),
                                                          to_inject_mv_x,
                                                          to_inject_mv_y,
                                                          mi_col,
                                                          mi_row,
                                                          context_ptr->blk_geom->bsize);
                uint8_t skip_cand = (!inside_tile);

                    MvReferenceFrame rf[2];
                    rf[0] = to_inject_ref_type;
                    rf[1] = -1;
                if (!skip_cand &&
                    (context_ptr->injected_mv_count_l0 == 0 ||
                     mrp_is_already_injected_mv_l0(
                         context_ptr, to_inject_mv_x, to_inject_mv_y, to_inject_ref_type) ==
                         EB_FALSE)) {
                    uint8_t inter_type;
#if FTR_NEW_REF_PRUNING_CTRLS // 3x3_unipred inter_intra
                    uint8_t is_ii_allowed = svt_is_interintra_allowed(context_ptr->inter_intra_comp_ctrls.enabled, context_ptr->blk_geom->bsize, NEWMV, rf);
#else
                    uint8_t is_ii_allowed = context_ptr->inter_intra_comp_ctrls.enabled ?
                        svt_is_interintra_allowed(!context_ptr->inter_intra_comp_ctrls.skip_pme_unipred, context_ptr->blk_geom->bsize, NEWMV, rf) :
                        0;

                    if (is_ii_allowed && context_ptr->inter_intra_comp_ctrls.closest_ref_only)
                        if (list0_ref_index > 0)
                            is_ii_allowed = 0;
#endif
                    uint8_t tot_inter_types = is_ii_allowed ? II_COUNT : 1;
                    //uint8_t is_obmc_allowed =  obmc_motion_mode_allowed(pcs_ptr, context_ptr->blk_ptr, bsize, rf[0], rf[1], NEWMV) == OBMC_CAUSAL;
                    //tot_inter_types = is_obmc_allowed ? tot_inter_types+1 : tot_inter_types;

#if FTR_UPGRADE_COMP_LEVELS
                    uint8_t drl_index = 0;
                    choose_best_av1_mv_pred(context_ptr,
                        context_ptr->md_rate_estimation_ptr,
                        context_ptr->blk_ptr,
                        to_inject_ref_type,
                        0,
                        NEWMV,
                        to_inject_mv_x,
                        to_inject_mv_y,
                        0,
                        0,
                        &drl_index,
                        best_pred_mv);
#endif
                    for (inter_type = 0; inter_type < tot_inter_types; inter_type++) {
#if FTR_NEW_REF_PRUNING_CTRLS
                        if (!is_valid_uni_type(context_ptr, inter_type, is_ii_allowed, 0, REF_LIST_0, list0_ref_index))
                            continue;
#endif
                        cand_array[cand_total_cnt].type                    = INTER_MODE;
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].distortion_ready        = 0;
#endif
                        cand_array[cand_total_cnt].use_intrabc             = 0;
                        cand_array[cand_total_cnt].skip_mode_allowed              = EB_FALSE;
                        cand_array[cand_total_cnt].prediction_direction[0] = (EbPredDirection)0;
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].inter_mode              = NEWMV;
#endif
                        cand_array[cand_total_cnt].pred_mode               = NEWMV;
                        cand_array[cand_total_cnt].motion_mode             = SIMPLE_TRANSLATION;

                        cand_array[cand_total_cnt].is_compound = 0;
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].is_new_mv   = 1;
#endif
#if FTR_UPGRADE_COMP_LEVELS
                        cand_array[cand_total_cnt].drl_index = drl_index;
#else
                        cand_array[cand_total_cnt].drl_index = 0;
#endif
                        // Set the MV to ME result
                        cand_array[cand_total_cnt].motion_vector_xl0 = to_inject_mv_x;
                        cand_array[cand_total_cnt].motion_vector_yl0 = to_inject_mv_y;

                        // will be needed later by the rate estimation
#if CLN_MD_CANDS
                        cand_array[cand_total_cnt].ref_frame_type = to_inject_ref_type;
#else
                        cand_array[cand_total_cnt].ref_mv_index   = 0;
                        cand_array[cand_total_cnt].ref_frame_type =
                            svt_get_ref_frame_type(REF_LIST_0, list0_ref_index);
                        cand_array[cand_total_cnt].ref_frame_index_l0 = list0_ref_index;
                        cand_array[cand_total_cnt].ref_frame_index_l1 = -1;
                        cand_array[cand_total_cnt].transform_type[0] = DCT_DCT;
                        cand_array[cand_total_cnt].transform_type_uv = DCT_DCT;
#endif
#if !FTR_UPGRADE_COMP_LEVELS
                        choose_best_av1_mv_pred(context_ptr,
#if CLN_FAST_COST
                                                context_ptr->md_rate_estimation_ptr,
#else
                                                cand_array[cand_total_cnt].md_rate_estimation_ptr,
#endif
                                                context_ptr->blk_ptr,
                                                cand_array[cand_total_cnt].ref_frame_type,
                                                cand_array[cand_total_cnt].is_compound,
                                                cand_array[cand_total_cnt].pred_mode,
                                                cand_array[cand_total_cnt].motion_vector_xl0,
                                                cand_array[cand_total_cnt].motion_vector_yl0,
                                                0,
                                                0,
                                                &cand_array[cand_total_cnt].drl_index,
                                                best_pred_mv);
#endif
                        cand_array[cand_total_cnt].motion_vector_pred_x[REF_LIST_0] =
                            best_pred_mv[0].as_mv.col;
                        cand_array[cand_total_cnt].motion_vector_pred_y[REF_LIST_0] =
                            best_pred_mv[0].as_mv.row;
                        if (inter_type == 0) {
                            cand_array[cand_total_cnt].is_interintra_used = 0;
                            cand_array[cand_total_cnt].motion_mode        = SIMPLE_TRANSLATION;
                        } else {
                            if (is_ii_allowed) {
                                if (inter_type == 1) {
                                    inter_intra_search(
                                        pcs_ptr, context_ptr, &cand_array[cand_total_cnt]);
                                    cand_array[cand_total_cnt].is_interintra_used   = 1;
                                    cand_array[cand_total_cnt].use_wedge_interintra = 1;
                                } else if (inter_type == 2) {
                                    cand_array[cand_total_cnt].is_interintra_used = 1;
                                    cand_array[cand_total_cnt].interintra_mode =
                                        cand_array[cand_total_cnt - 1].interintra_mode;
                                    cand_array[cand_total_cnt].use_wedge_interintra = 0;
                                }
                            }
                            //if (is_obmc_allowed && inter_type == tot_inter_types - 1) {
                            //    cand_array[cand_total_cnt].is_interintra_used = 0;
                            //    cand_array[cand_total_cnt].motion_mode = OBMC_CAUSAL;
                            //}
                        }

                        INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
                    }
                    context_ptr->injected_mv_x_l0_array[context_ptr->injected_mv_count_l0] =
                        to_inject_mv_x;
                    context_ptr->injected_mv_y_l0_array[context_ptr->injected_mv_count_l0] =
                        to_inject_mv_y;
                    context_ptr->injected_ref_type_l0_array[context_ptr->injected_mv_count_l0] =
                        to_inject_ref_type;
                    ++context_ptr->injected_mv_count_l0;
                }
            }
        }
    }

    // (8 Best_L1 neighbors)
    for (uint8_t me_candidate_index = 0; me_candidate_index < total_me_cnt; ++me_candidate_index) {
        const MeCandidate *me_block_results_ptr = &me_block_results[me_candidate_index];
        const uint8_t      inter_direction      = me_block_results_ptr->direction;
        const uint8_t      list1_ref_index      = me_block_results_ptr->ref_idx_l1;
        if (inter_direction == 1) {
        if (!is_valid_unipred_ref(context_ptr, MIN(TOT_INTER_GROUP-1,UNI_3x3_GROUP), REF_LIST_1, list1_ref_index)) continue;
#if !FTR_NEW_REF_PRUNING_CTRLS
            if (list1_ref_index > context_ptr->md_max_ref_count - 1)
                continue;
#endif
            for (bipred_index = 0; bipred_index < BIPRED_3x3_REFINMENT_POSITIONS; ++bipred_index) {
                if (is_compound_enabled) {
                    /**************
            NEWMV L1
            ************* */
                    if (context_ptr->unipred3x3_injection >= 2) {
                        if (allow_refinement_flag[bipred_index] == 0) continue;
                    }
                    int16_t to_inject_mv_x;
                    int16_t to_inject_mv_y;
                    if (pcs_ptr->parent_pcs_ptr->frm_hdr.allow_high_precision_mv) {
                        to_inject_mv_x = context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                            [REF_LIST_1][list1_ref_index][0] +
                            bipred_3x3_x_pos[bipred_index];
                        to_inject_mv_y = context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                            [REF_LIST_1][list1_ref_index][1] +
                            bipred_3x3_y_pos[bipred_index];
                    }
                    else {
                        to_inject_mv_x = context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                            [REF_LIST_1][list1_ref_index][0] +
                            (bipred_3x3_x_pos[bipred_index] << 1);
                        to_inject_mv_y = context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                            [REF_LIST_1][list1_ref_index][1] +
                            (bipred_3x3_y_pos[bipred_index] << 1);
                    }
                    uint8_t to_inject_ref_type =
                        svt_get_ref_frame_type(REF_LIST_1, list1_ref_index);

                    inside_tile = 1;
                    if (umv0tile)
                        inside_tile = is_inside_tile_boundary(&(xd->tile),
                                                              to_inject_mv_x,
                                                              to_inject_mv_y,
                                                              mi_col,
                                                              mi_row,
                                                              context_ptr->blk_geom->bsize);
                    uint8_t skip_cand = (!inside_tile);

                    MvReferenceFrame rf[2];
                    rf[0] = to_inject_ref_type;
                    rf[1] = -1;
                    if (!skip_cand &&
                        (context_ptr->injected_mv_count_l1 == 0 ||
                         mrp_is_already_injected_mv_l1(
                             context_ptr, to_inject_mv_x, to_inject_mv_y, to_inject_ref_type) ==
                             EB_FALSE)) {
                        uint8_t inter_type;
#if FTR_NEW_REF_PRUNING_CTRLS // 3x3_unipred inter_intra
                        uint8_t is_ii_allowed = svt_is_interintra_allowed(context_ptr->inter_intra_comp_ctrls.enabled, context_ptr->blk_geom->bsize, NEWMV, rf);
#else
                        uint8_t is_ii_allowed = context_ptr->inter_intra_comp_ctrls.enabled ?
                            svt_is_interintra_allowed(!context_ptr->inter_intra_comp_ctrls.skip_pme_unipred, context_ptr->blk_geom->bsize, NEWMV, rf) :
                            0;

                        if (is_ii_allowed && context_ptr->inter_intra_comp_ctrls.closest_ref_only)
                            if (list1_ref_index > 0)
                                is_ii_allowed = 0;
#endif
                        uint8_t tot_inter_types = is_ii_allowed ? II_COUNT : 1;
#if FTR_UPGRADE_COMP_LEVELS
                        uint8_t drl_index = 0;
                        choose_best_av1_mv_pred(context_ptr,
                            context_ptr->md_rate_estimation_ptr,
                            context_ptr->blk_ptr,
                            to_inject_ref_type,
                            0,
                            NEWMV,
                            to_inject_mv_x,
                            to_inject_mv_y,
                            0,
                            0,
                            &drl_index,
                            best_pred_mv);
#endif
                        for (inter_type = 0; inter_type < tot_inter_types; inter_type++) {
#if FTR_NEW_REF_PRUNING_CTRLS
                            if (!is_valid_uni_type(context_ptr, inter_type, is_ii_allowed, 0, REF_LIST_1, list1_ref_index))
                                continue;
#endif
                            cand_array[cand_total_cnt].type                    = INTER_MODE;
#if !CLN_MD_CANDS
                            cand_array[cand_total_cnt].distortion_ready        = 0;
#endif
                            cand_array[cand_total_cnt].use_intrabc             = 0;
                            cand_array[cand_total_cnt].skip_mode_allowed              = EB_FALSE;
                            cand_array[cand_total_cnt].prediction_direction[0] = (EbPredDirection)1;
#if !CLN_MD_CANDS
                            cand_array[cand_total_cnt].inter_mode              = NEWMV;
#endif
                            cand_array[cand_total_cnt].pred_mode               = NEWMV;
                            cand_array[cand_total_cnt].motion_mode             = SIMPLE_TRANSLATION;

                            cand_array[cand_total_cnt].is_compound = 0;
#if !CLN_MD_CANDS
                            cand_array[cand_total_cnt].is_new_mv   = 1;
#endif
#if FTR_UPGRADE_COMP_LEVELS
                            cand_array[cand_total_cnt].drl_index = drl_index;
#else
                            cand_array[cand_total_cnt].drl_index = 0;
#endif
                            // Set the MV to ME result
                            cand_array[cand_total_cnt].motion_vector_xl1 = to_inject_mv_x;
                            cand_array[cand_total_cnt].motion_vector_yl1 = to_inject_mv_y;
                            // will be needed later by the rate estimation
#if CLN_MD_CANDS
                            cand_array[cand_total_cnt].ref_frame_type = to_inject_ref_type;
#else
                            cand_array[cand_total_cnt].ref_mv_index   = 0;
                            cand_array[cand_total_cnt].ref_frame_type =
                                svt_get_ref_frame_type(REF_LIST_1, list1_ref_index);
                            cand_array[cand_total_cnt].ref_frame_index_l0 = -1;
                            cand_array[cand_total_cnt].ref_frame_index_l1 = list1_ref_index;
                            cand_array[cand_total_cnt].transform_type[0]  = DCT_DCT;
                            cand_array[cand_total_cnt].transform_type_uv  = DCT_DCT;
#endif
#if !FTR_UPGRADE_COMP_LEVELS
                            choose_best_av1_mv_pred(
                                context_ptr,
#if CLN_FAST_COST
                                context_ptr->md_rate_estimation_ptr,
#else
                                cand_array[cand_total_cnt].md_rate_estimation_ptr,
#endif
                                context_ptr->blk_ptr,
                                cand_array[cand_total_cnt].ref_frame_type,
                                cand_array[cand_total_cnt].is_compound,
                                cand_array[cand_total_cnt].pred_mode,
                                cand_array[cand_total_cnt].motion_vector_xl1,
                                cand_array[cand_total_cnt].motion_vector_yl1,
                                0,
                                0,
                                &cand_array[cand_total_cnt].drl_index,
                                best_pred_mv);
#endif
                            cand_array[cand_total_cnt].motion_vector_pred_x[REF_LIST_1] =
                                best_pred_mv[0].as_mv.col;
                            cand_array[cand_total_cnt].motion_vector_pred_y[REF_LIST_1] =
                                best_pred_mv[0].as_mv.row;
                            if (inter_type == 0) {
                                cand_array[cand_total_cnt].is_interintra_used = 0;
                                cand_array[cand_total_cnt].motion_mode        = SIMPLE_TRANSLATION;
                            } else {
                                if (is_ii_allowed) {
                                    if (inter_type == 1) {
                                        inter_intra_search(
                                            pcs_ptr, context_ptr, &cand_array[cand_total_cnt]);
                                        cand_array[cand_total_cnt].is_interintra_used   = 1;
                                        cand_array[cand_total_cnt].use_wedge_interintra = 1;
                                    } else if (inter_type == 2) {
                                        cand_array[cand_total_cnt].is_interintra_used = 1;
                                        cand_array[cand_total_cnt].interintra_mode =
                                            cand_array[cand_total_cnt - 1].interintra_mode;
                                        cand_array[cand_total_cnt].use_wedge_interintra = 0;
                                    }
                                }
                                //if (is_obmc_allowed && inter_type == tot_inter_types - 1) {
                                //    cand_array[cand_total_cnt].is_interintra_used = 0;
                                //    cand_array[cand_total_cnt].motion_mode = OBMC_CAUSAL;
                                //}
                            }

                            INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
                        }
                        context_ptr->injected_mv_x_l1_array[context_ptr->injected_mv_count_l1] =
                            to_inject_mv_x;
                        context_ptr->injected_mv_y_l1_array[context_ptr->injected_mv_count_l1] =
                            to_inject_mv_y;
                        context_ptr->injected_ref_type_l1_array[context_ptr->injected_mv_count_l1] =
                            to_inject_ref_type;
                        ++context_ptr->injected_mv_count_l1;
                    }
                }
            }
        }
    }

    // update the total number of candidates injected
    (*candidate_total_cnt) = cand_total_cnt;

    return;
}

/*
 * This function configures the compound modes to be injected
 */
void set_compound_to_inject(ModeDecisionContext *context_ptr, EbBool * comp_inj_table,
                            EbBool avg, EbBool dist, EbBool diff, EbBool wdg) {

    if (get_wedge_params_bits(context_ptr->blk_geom->bsize) == 0)
        wdg = 0;

    comp_inj_table[MD_COMP_AVG] = avg;
    comp_inj_table[MD_COMP_DIST] = dist;
    comp_inj_table[MD_COMP_DIFF0] = diff;
    comp_inj_table[MD_COMP_WEDGE] = wdg;
}
#if !FTR_NEW_REF_PRUNING_CTRLS
void set_comp_types_by_distance(uint8_t allowed_comp_types[MD_COMP_TYPES],
                                uint8_t allowed_dist1_comp_types[MD_COMP_TYPES],
                                uint8_t allowed_dist2_comp_types[MD_COMP_TYPES],
                                uint8_t list0_ref_idx,
                                uint8_t list1_ref_idx) {

    if (list0_ref_idx > 1 || list1_ref_idx > 1) {
        // distance 2 compound types should be a subset of the allowed compound types
        for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < MD_COMP_TYPES; cur_type++)
            allowed_comp_types[cur_type] &= allowed_dist2_comp_types[cur_type];
    }
    else if (list0_ref_idx > 0 || list1_ref_idx > 0) {
        // distance 1 compound types should be a subset of the allowed compound types
        for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < MD_COMP_TYPES; cur_type++)
            allowed_comp_types[cur_type] &= allowed_dist1_comp_types[cur_type];
    }
}
#endif
void bipred_3x3_candidates_injection(const SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr,
                                     ModeDecisionContext *context_ptr, SuperBlock *sb_ptr,
                                     uint32_t me_sb_addr, uint32_t *candidate_total_cnt) {
    UNUSED(sb_ptr);
    uint32_t           cand_total_cnt = (*candidate_total_cnt);
    FrameHeader *      frm_hdr        = &pcs_ptr->parent_pcs_ptr->frm_hdr;
    const MeSbResults *me_results = pcs_ptr->parent_pcs_ptr->pa_me_data->me_results[me_sb_addr];
    uint8_t total_me_cnt = me_results->total_me_candidate_index[context_ptr->me_block_offset];
    const MeCandidate *me_block_results = &me_results->me_candidate_array[context_ptr->me_cand_offset];
    ModeDecisionCandidate *cand_array   = context_ptr->fast_candidate_array;
    EbBool       is_compound_enabled    = (frm_hdr->reference_mode == SINGLE_REFERENCE) ? 0 : 1;
    IntMv        best_pred_mv[2]        = {{0}, {0}};
    MacroBlockD *xd                     = context_ptr->blk_ptr->av1xd;
    int          umv0tile               = (scs_ptr->static_config.unrestricted_motion_vector == 0);
    uint32_t     mi_row                 = context_ptr->blk_origin_y >> MI_SIZE_LOG2;
    uint32_t     mi_col                 = context_ptr->blk_origin_x >> MI_SIZE_LOG2;

    if (is_compound_enabled) {
#if FTR_UPGRADE_COMP_LEVELS
        MD_COMP_TYPE tot_comp_types =
            (context_ptr->inter_comp_ctrls.do_3x3_bi == 0)
            ? MD_COMP_DIST
            : MD_COMP_TYPES;
#endif
        /**************
       NEW_NEWMV
       ************* */
        for (uint8_t me_candidate_index = 0; me_candidate_index < total_me_cnt;
             ++me_candidate_index) {
            const MeCandidate *me_block_results_ptr = &me_block_results[me_candidate_index];
            const uint8_t      inter_direction      = me_block_results_ptr->direction;
            const uint8_t      list0_ref_index      = me_block_results_ptr->ref_idx_l0;
            const uint8_t      list1_ref_index      = me_block_results_ptr->ref_idx_l1;
           if (inter_direction == 2) {
           if (!is_valid_bipred_ref(
                context_ptr, BI_3x3_GROUP, me_block_results_ptr->ref0_list, list0_ref_index, me_block_results_ptr->ref1_list, list1_ref_index)) continue;
#if !FTR_NEW_REF_PRUNING_CTRLS
                if (list0_ref_index > context_ptr->md_max_ref_count - 1 ||
                    list1_ref_index > context_ptr->md_max_ref_count - 1)
                    continue;
#endif
                // (Best_L0, 8 Best_L1 neighbors)
                for (uint32_t bipred_index = 0; bipred_index < BIPRED_3x3_REFINMENT_POSITIONS;
                     ++bipred_index) {
                    if (context_ptr->bipred3x3_injection >= 2) {
                        if (allow_refinement_flag[bipred_index] == 0) continue;
                    }
                    int16_t to_inject_mv_x_l0 =
                        context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                        [me_block_results_ptr->ref0_list][list0_ref_index][0];
                    int16_t to_inject_mv_y_l0 =
                        context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                        [me_block_results_ptr->ref0_list][list0_ref_index][1];

                    int16_t to_inject_mv_x_l1;
                    int16_t to_inject_mv_y_l1;
                    if (pcs_ptr->parent_pcs_ptr->frm_hdr.allow_high_precision_mv) {
                        to_inject_mv_x_l1 =
                            context_ptr
                            ->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                            [me_block_results_ptr->ref1_list][list1_ref_index][0] +
                            bipred_3x3_x_pos[bipred_index];
                        to_inject_mv_y_l1 =
                            context_ptr
                            ->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                            [me_block_results_ptr->ref1_list][list1_ref_index][1] +
                            bipred_3x3_y_pos[bipred_index];
                    }
                    else {
                        to_inject_mv_x_l1 =
                            context_ptr
                            ->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                            [me_block_results_ptr->ref1_list][list1_ref_index][0] +
                            (bipred_3x3_x_pos[bipred_index] << 1);
                        to_inject_mv_y_l1 =
                            context_ptr
                            ->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                            [me_block_results_ptr->ref1_list][list1_ref_index][1] +
                            (bipred_3x3_y_pos[bipred_index] << 1);
                    }

                    uint8_t to_inject_ref_type = av1_ref_frame_type((const MvReferenceFrame[]){
                        svt_get_ref_frame_type(me_block_results_ptr->ref0_list, list0_ref_index),
                        svt_get_ref_frame_type(me_block_results_ptr->ref1_list, list1_ref_index)});

                    int inside_tile = umv0tile ? is_inside_tile_boundary(&(xd->tile),
                                                              to_inject_mv_x_l0,
                                                              to_inject_mv_y_l0,
                                                              mi_col,
                                                              mi_row,
                                                              context_ptr->blk_geom->bsize)
                                               : 1;
                    uint8_t skip_cand = (!inside_tile);
                    if (!skip_cand &&
                        (context_ptr->injected_mv_count_bipred == 0 ||
                         mrp_is_already_injected_mv_bipred(context_ptr,
                                                           to_inject_mv_x_l0,
                                                           to_inject_mv_y_l0,
                                                           to_inject_mv_x_l1,
                                                           to_inject_mv_y_l1,
                                                           to_inject_ref_type) == EB_FALSE)) {
                        // Set the allowable compound types to be injected
                        uint8_t allowed_comp_types[MD_COMP_TYPES];
                        memcpy(allowed_comp_types, context_ptr->inter_comp_ctrls.allowed_comp_types, sizeof(uint8_t) * MD_COMP_TYPES);
#if !FTR_NEW_REF_PRUNING_CTRLS
                        // Reduce the compound types to inject based on the distance of the reference frames from the current frame
                        set_comp_types_by_distance(allowed_comp_types,
                                                   context_ptr->inter_comp_ctrls.allowed_dist1_comp_types,
                                                   context_ptr->inter_comp_ctrls.allowed_dist2_comp_types,
                                                   list0_ref_index,
                                                   list1_ref_index);
#endif
                        EbBool mask_done = 0;
#if FTR_UPGRADE_COMP_LEVELS
                        uint8_t drl_index = 0;
                        choose_best_av1_mv_pred(
                            context_ptr,
                            context_ptr->md_rate_estimation_ptr,
                            context_ptr->blk_ptr,
                            to_inject_ref_type,
                            1,
                            NEW_NEWMV,
                            to_inject_mv_x_l0,
                            to_inject_mv_y_l0,
                            to_inject_mv_x_l1,
                            to_inject_mv_y_l1,
                            &drl_index,
                            best_pred_mv);

                        for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < tot_comp_types; cur_type++) {
#else
                        for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < MD_COMP_TYPES; cur_type++) {
#endif

                            // Skip the current compound type if not set to be injected
                            if (allowed_comp_types[cur_type] == 0)
                                continue;
#if FTR_NEW_REF_PRUNING_CTRLS
                            if (!is_valid_bi_type(context_ptr, cur_type, me_block_results_ptr->ref0_list, list0_ref_index, me_block_results_ptr->ref1_list, list1_ref_index))
                                continue;
#endif
                            cand_array[cand_total_cnt].type             = INTER_MODE;
#if !CLN_MD_CANDS
                            cand_array[cand_total_cnt].distortion_ready = 0;
#endif
                            cand_array[cand_total_cnt].use_intrabc      = 0;
                            cand_array[cand_total_cnt].skip_mode_allowed       = EB_FALSE;
#if !CLN_MD_CANDS
                            cand_array[cand_total_cnt].is_new_mv        = 1;
#endif
#if FTR_UPGRADE_COMP_LEVELS
                            cand_array[cand_total_cnt].drl_index = drl_index;
#else
                            cand_array[cand_total_cnt].drl_index = 0;
#endif

                            // Set the MV to ME result
                            cand_array[cand_total_cnt].motion_vector_xl0 = to_inject_mv_x_l0;
                            cand_array[cand_total_cnt].motion_vector_yl0 = to_inject_mv_y_l0;
                            cand_array[cand_total_cnt].motion_vector_xl1 = to_inject_mv_x_l1;
                            cand_array[cand_total_cnt].motion_vector_yl1 = to_inject_mv_y_l1;
                            // will be needed later by the rate estimation
#if !CLN_MD_CANDS
                            cand_array[cand_total_cnt].ref_mv_index            = 0;
                            cand_array[cand_total_cnt].inter_mode              = NEW_NEWMV;
#endif
                            cand_array[cand_total_cnt].pred_mode               = NEW_NEWMV;
                            cand_array[cand_total_cnt].motion_mode             = SIMPLE_TRANSLATION;
                            cand_array[cand_total_cnt].is_compound             = 1;
                            cand_array[cand_total_cnt].is_interintra_used      = 0;
                            cand_array[cand_total_cnt].prediction_direction[0] = (EbPredDirection)2;
#if CLN_MD_CANDS
                            cand_array[cand_total_cnt].ref_frame_type = to_inject_ref_type;
#else
                            cand_array[cand_total_cnt].ref_frame_type = av1_ref_frame_type(
                                (const MvReferenceFrame[]){
                                    svt_get_ref_frame_type(me_block_results_ptr->ref0_list,
                                                           list0_ref_index),
                                    svt_get_ref_frame_type(me_block_results_ptr->ref1_list,
                                                           list1_ref_index)});

                            cand_array[cand_total_cnt].ref_frame_index_l0 = list0_ref_index;
                            cand_array[cand_total_cnt].ref_frame_index_l1 = list1_ref_index;
                            cand_array[cand_total_cnt].transform_type[0]  = DCT_DCT;
                            cand_array[cand_total_cnt].transform_type_uv  = DCT_DCT;
#endif
#if !FTR_UPGRADE_COMP_LEVELS
                            choose_best_av1_mv_pred(
                                context_ptr,
#if CLN_FAST_COST
                                context_ptr->md_rate_estimation_ptr,
#else
                                cand_array[cand_total_cnt].md_rate_estimation_ptr,
#endif
                                context_ptr->blk_ptr,
                                cand_array[cand_total_cnt].ref_frame_type,
                                cand_array[cand_total_cnt].is_compound,
                                cand_array[cand_total_cnt].pred_mode,
                                cand_array[cand_total_cnt].motion_vector_xl0,
                                cand_array[cand_total_cnt].motion_vector_yl0,
                                cand_array[cand_total_cnt].motion_vector_xl1,
                                cand_array[cand_total_cnt].motion_vector_yl1,
                                &cand_array[cand_total_cnt].drl_index,
                                best_pred_mv);
#endif
                            cand_array[cand_total_cnt].motion_vector_pred_x[REF_LIST_0] =
                                best_pred_mv[0].as_mv.col;
                            cand_array[cand_total_cnt].motion_vector_pred_y[REF_LIST_0] =
                                best_pred_mv[0].as_mv.row;
                            cand_array[cand_total_cnt].motion_vector_pred_x[REF_LIST_1] =
                                best_pred_mv[1].as_mv.col;
                            cand_array[cand_total_cnt].motion_vector_pred_y[REF_LIST_1] =
                                best_pred_mv[1].as_mv.row;
#if FTR_UPGRADE_COMP_LEVELS
                            if (cur_type > MD_COMP_AVG) {
                                if (mask_done != 1) {
                                    if (calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_total_cnt]))
                                        break;

                                    mask_done = 1;
                                }
                            }
#else
                            if (cur_type == MD_COMP_DIFF0 || cur_type == MD_COMP_WEDGE) {
                                if (mask_done != 1) {
                                    calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_total_cnt]);
                                    mask_done = 1;
                                }
                            }
#endif
                            //BIP 3x3
                            determine_compound_mode(
                                pcs_ptr, context_ptr, &cand_array[cand_total_cnt], cur_type);
                            INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
#if !FTR_UPGRADE_COMP_LEVELS
                            context_ptr->injected_mv_x_bipred_l0_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l0;
                            context_ptr->injected_mv_y_bipred_l0_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l0;
                            context_ptr->injected_mv_x_bipred_l1_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l1;
                            context_ptr->injected_mv_y_bipred_l1_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l1;
                            context_ptr->injected_ref_type_bipred_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_ref_type;
                            ++context_ptr->injected_mv_count_bipred;
#endif
                        }
#if FTR_UPGRADE_COMP_LEVELS
                        context_ptr->injected_mv_x_bipred_l0_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l0;
                        context_ptr->injected_mv_y_bipred_l0_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l0;
                        context_ptr->injected_mv_x_bipred_l1_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l1;
                        context_ptr->injected_mv_y_bipred_l1_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l1;
                        context_ptr->injected_ref_type_bipred_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_ref_type;
                        ++context_ptr->injected_mv_count_bipred;
#endif
                    }
                }

                // (8 Best_L0 neighbors, Best_L1) :
                for (uint32_t bipred_index = 0; bipred_index < BIPRED_3x3_REFINMENT_POSITIONS;
                     ++bipred_index) {
                    if (context_ptr->bipred3x3_injection >= 2) {
                        if (allow_refinement_flag[bipred_index] == 0) continue;
                    }

                    int16_t to_inject_mv_x_l0;
                    int16_t to_inject_mv_y_l0;
                    if (pcs_ptr->parent_pcs_ptr->frm_hdr.allow_high_precision_mv) {
                        to_inject_mv_x_l0 =
                            context_ptr
                            ->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                            [me_block_results_ptr->ref0_list][list0_ref_index][0] +
                            bipred_3x3_x_pos[bipred_index];
                        to_inject_mv_y_l0 =
                            context_ptr
                            ->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                            [me_block_results_ptr->ref0_list][list0_ref_index][1] +
                            bipred_3x3_y_pos[bipred_index];
                    }
                    else {
                        to_inject_mv_x_l0 =
                            context_ptr
                            ->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                            [me_block_results_ptr->ref0_list][list0_ref_index][0] +
                            (bipred_3x3_x_pos[bipred_index] << 1);
                        to_inject_mv_y_l0 =
                            context_ptr
                            ->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                            [me_block_results_ptr->ref0_list][list0_ref_index][1] +
                            (bipred_3x3_y_pos[bipred_index] << 1);
                    }
                    int16_t to_inject_mv_x_l1 =
                        context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                        [me_block_results_ptr->ref1_list][list1_ref_index][0];
                    int16_t to_inject_mv_y_l1 =
                        context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                        [me_block_results_ptr->ref1_list][list1_ref_index][1];
                    uint8_t to_inject_ref_type = av1_ref_frame_type((const MvReferenceFrame[]){
                        svt_get_ref_frame_type(me_block_results_ptr->ref0_list, list0_ref_index),
                        svt_get_ref_frame_type(me_block_results_ptr->ref1_list, list1_ref_index)});

                    int inside_tile =  umv0tile ? is_inside_tile_boundary(&(xd->tile),
                                                              to_inject_mv_x_l0,
                                                              to_inject_mv_y_l0,
                                                              mi_col,
                                                              mi_row,
                                                              context_ptr->blk_geom->bsize) &&
                                      is_inside_tile_boundary(&(xd->tile),
                                                              to_inject_mv_x_l1,
                                                              to_inject_mv_y_l1,
                                                              mi_col,
                                                              mi_row,
                                                              context_ptr->blk_geom->bsize)
                                                : 1;
                    uint8_t skip_cand = (!inside_tile);
                    if (!skip_cand &&
                        (context_ptr->injected_mv_count_bipred == 0 ||
                         mrp_is_already_injected_mv_bipred(context_ptr,
                                                           to_inject_mv_x_l0,
                                                           to_inject_mv_y_l0,
                                                           to_inject_mv_x_l1,
                                                           to_inject_mv_y_l1,
                                                           to_inject_ref_type) == EB_FALSE)) {
                        // Set the allowable compound types to be injected
                        uint8_t allowed_comp_types[MD_COMP_TYPES];
                        memcpy(allowed_comp_types, context_ptr->inter_comp_ctrls.allowed_comp_types, sizeof(uint8_t) * MD_COMP_TYPES);
#if !FTR_NEW_REF_PRUNING_CTRLS
                        // Reduce the compound types to inject based on the distance of the reference frames from the current frame
                        set_comp_types_by_distance(allowed_comp_types,
                            context_ptr->inter_comp_ctrls.allowed_dist1_comp_types,
                            context_ptr->inter_comp_ctrls.allowed_dist2_comp_types,
                            list0_ref_index,
                            list1_ref_index);
#endif
                        EbBool mask_done = 0;
#if FTR_UPGRADE_COMP_LEVELS
                        uint8_t drl_index = 0;
                        choose_best_av1_mv_pred(
                            context_ptr,
                            context_ptr->md_rate_estimation_ptr,
                            context_ptr->blk_ptr,
                            to_inject_ref_type,
                            1,
                            NEW_NEWMV,
                            to_inject_mv_x_l0,
                            to_inject_mv_y_l0,
                            to_inject_mv_x_l1,
                            to_inject_mv_y_l1,
                            &drl_index,
                            best_pred_mv);

                        for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < tot_comp_types; cur_type++) {
#else
                        for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < MD_COMP_TYPES; cur_type++) {
#endif

                            // Skip the current compound type if not set to be injected
                            if (allowed_comp_types[cur_type] == 0)
                                continue;
#if FTR_NEW_REF_PRUNING_CTRLS
                            if (!is_valid_bi_type(context_ptr, cur_type, me_block_results_ptr->ref0_list, list0_ref_index, me_block_results_ptr->ref1_list, list1_ref_index))
                                continue;
#endif
                            cand_array[cand_total_cnt].type             = INTER_MODE;
#if !CLN_MD_CANDS
                            cand_array[cand_total_cnt].distortion_ready = 0;
#endif
                            cand_array[cand_total_cnt].use_intrabc      = 0;
                            cand_array[cand_total_cnt].skip_mode_allowed       = EB_FALSE;
#if !CLN_MD_CANDS
                            cand_array[cand_total_cnt].is_new_mv  = 1;
#endif
#if FTR_UPGRADE_COMP_LEVELS
                            cand_array[cand_total_cnt].drl_index = drl_index;
#else
                            cand_array[cand_total_cnt].drl_index = 0;
#endif

                            // Set the MV to ME result
                            cand_array[cand_total_cnt].motion_vector_xl0 = to_inject_mv_x_l0;
                            cand_array[cand_total_cnt].motion_vector_yl0 = to_inject_mv_y_l0;
                            cand_array[cand_total_cnt].motion_vector_xl1 = to_inject_mv_x_l1;
                            cand_array[cand_total_cnt].motion_vector_yl1 = to_inject_mv_y_l1;
                            // will be needed later by the rate estimation
#if !CLN_MD_CANDS
                            cand_array[cand_total_cnt].ref_mv_index            = 0;
                            cand_array[cand_total_cnt].inter_mode              = NEW_NEWMV;
#endif
                            cand_array[cand_total_cnt].pred_mode               = NEW_NEWMV;
                            cand_array[cand_total_cnt].motion_mode             = SIMPLE_TRANSLATION;
                            cand_array[cand_total_cnt].is_compound             = 1;
                            cand_array[cand_total_cnt].is_interintra_used      = 0;
                            cand_array[cand_total_cnt].prediction_direction[0] = (EbPredDirection)2;
#if CLN_MD_CANDS
                            cand_array[cand_total_cnt].ref_frame_type = to_inject_ref_type;
#else
                            cand_array[cand_total_cnt].ref_frame_type = av1_ref_frame_type(
                                (const MvReferenceFrame[]){
                                    svt_get_ref_frame_type(me_block_results_ptr->ref0_list,
                                                           list0_ref_index),
                                    svt_get_ref_frame_type(me_block_results_ptr->ref1_list,
                                                           list1_ref_index)});

                            cand_array[cand_total_cnt].ref_frame_index_l0 = list0_ref_index;
                            cand_array[cand_total_cnt].ref_frame_index_l1 = list1_ref_index;
                            cand_array[cand_total_cnt].transform_type[0]  = DCT_DCT;
                            cand_array[cand_total_cnt].transform_type_uv  = DCT_DCT;
#endif
#if !FTR_UPGRADE_COMP_LEVELS
                            choose_best_av1_mv_pred(
                                context_ptr,
#if CLN_FAST_COST
                                context_ptr->md_rate_estimation_ptr,
#else
                                cand_array[cand_total_cnt].md_rate_estimation_ptr,
#endif
                                context_ptr->blk_ptr,
                                cand_array[cand_total_cnt].ref_frame_type,
                                cand_array[cand_total_cnt].is_compound,
                                cand_array[cand_total_cnt].pred_mode,
                                cand_array[cand_total_cnt].motion_vector_xl0,
                                cand_array[cand_total_cnt].motion_vector_yl0,
                                cand_array[cand_total_cnt].motion_vector_xl1,
                                cand_array[cand_total_cnt].motion_vector_yl1,
                                &cand_array[cand_total_cnt].drl_index,
                                best_pred_mv);
#endif
                            cand_array[cand_total_cnt].motion_vector_pred_x[REF_LIST_0] =
                                best_pred_mv[0].as_mv.col;
                            cand_array[cand_total_cnt].motion_vector_pred_y[REF_LIST_0] =
                                best_pred_mv[0].as_mv.row;
                            cand_array[cand_total_cnt].motion_vector_pred_x[REF_LIST_1] =
                                best_pred_mv[1].as_mv.col;
                            cand_array[cand_total_cnt].motion_vector_pred_y[REF_LIST_1] =
                                best_pred_mv[1].as_mv.row;
#if FTR_UPGRADE_COMP_LEVELS
                            if (cur_type > MD_COMP_AVG) {
                                if (mask_done != 1) {
                                    if (calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_total_cnt]))
                                        break;
                                    mask_done = 1;
                                }
                            }
#else
                            if (cur_type == MD_COMP_DIFF0 || cur_type == MD_COMP_WEDGE) {
                                if (mask_done != 1) {
                                    calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_total_cnt]);
                                    mask_done = 1;
                                }
                            }
#endif
                            //BIP 3x3
                            determine_compound_mode(
                                pcs_ptr, context_ptr, &cand_array[cand_total_cnt], cur_type);
                            INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
#if !FTR_UPGRADE_COMP_LEVELS
                            context_ptr->injected_mv_x_bipred_l0_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l0;
                            context_ptr->injected_mv_y_bipred_l0_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l0;
                            context_ptr->injected_mv_x_bipred_l1_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l1;
                            context_ptr->injected_mv_y_bipred_l1_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l1;
                            context_ptr->injected_ref_type_bipred_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_ref_type;
                            ++context_ptr->injected_mv_count_bipred;
#endif
                        }
#if FTR_UPGRADE_COMP_LEVELS
                        context_ptr->injected_mv_x_bipred_l0_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l0;
                        context_ptr->injected_mv_y_bipred_l0_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l0;
                        context_ptr->injected_mv_x_bipred_l1_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l1;
                        context_ptr->injected_mv_y_bipred_l1_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l1;
                        context_ptr->injected_ref_type_bipred_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_ref_type;
                        ++context_ptr->injected_mv_count_bipred;
#endif
                    }
                }
            }
        }
    }

    // update the total number of candidates injected
    (*candidate_total_cnt) = cand_total_cnt;

    return;
}

uint8_t get_max_drl_index(uint8_t refmvCnt, PredictionMode mode) {
    uint8_t max_drl = 0;

    if (mode == NEWMV || mode == NEW_NEWMV) {
        if (refmvCnt < 2)
            max_drl = 1;
        else if (refmvCnt == 2)
            max_drl = 2;
        else
            max_drl = 3;
    }

    if (mode == NEARMV || mode == NEAR_NEARMV || mode == NEAR_NEWMV || mode == NEW_NEARMV) {
        if (refmvCnt < 3)
            max_drl = 1;
        else if (refmvCnt == 3)
            max_drl = 2;
        else
            max_drl = 3;
    }

    return max_drl;
}
/*********************************************************************
**********************************************************************
        Upto 12 inter Candidated injected
        Min 6 inter Candidated injected
UniPred L0 : NEARST         + upto 3x NEAR
UniPred L1 : NEARST         + upto 3x NEAR
BIPred     : NEARST_NEARST  + upto 3x NEAR_NEAR
**********************************************************************
**********************************************************************/
#if CLN_MD_CANDS
void inject_mvp_candidates_ii(const SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr, uint32_t *candTotCnt) {
    BlkStruct *blk_ptr = context_ptr->blk_ptr;
#else
void inject_mvp_candidates_ii(struct ModeDecisionContext *context_ptr, PictureControlSet *pcs_ptr,
                              BlkStruct *blk_ptr, MvReferenceFrame ref_pair,
                              uint32_t *candTotCnt) {
#endif
    FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;
    EbBool       allow_compound =
        (frm_hdr->reference_mode == SINGLE_REFERENCE || context_ptr->blk_geom->bwidth == 4 ||
         context_ptr->blk_geom->bheight == 4)
            ? EB_FALSE
            : EB_TRUE;
    uint8_t                inj_mv;
    uint32_t               cand_idx   = *candTotCnt;
    ModeDecisionCandidate *cand_array = context_ptr->fast_candidate_array;
    MacroBlockD *          xd         = blk_ptr->av1xd;
    uint8_t                drli, max_drl_index;
    IntMv                  nearestmv[2], nearmv[2], ref_mv[2];
#if !CLN_MD_CANDS
    MvReferenceFrame    rf[2];
#endif
    int                 inside_tile = 1;
#if !CLN_MD_CANDS
    SequenceControlSet *scs_ptr =
        (SequenceControlSet *)pcs_ptr->parent_pcs_ptr->scs_wrapper_ptr->object_ptr;
#endif
    int      umv0tile = (scs_ptr->static_config.unrestricted_motion_vector == 0);
    uint32_t mi_row   = context_ptr->blk_origin_y >> MI_SIZE_LOG2;
    uint32_t mi_col   = context_ptr->blk_origin_x >> MI_SIZE_LOG2;
#if !CLN_MD_CANDS
    av1_set_ref_frame(rf, ref_pair);
#endif
    BlockSize    bsize          = context_ptr->blk_geom->bsize; // bloc size
#if CLN_MD_CANDS
    //all of ref pairs: (1)single-ref List0  (2)single-ref List1  (3)compound Bi-Dir List0-List1  (4)compound Uni-Dir List0-List0  (5)compound Uni-Dir List1-List1
    for (uint32_t ref_it = 0; ref_it < pcs_ptr->parent_pcs_ptr->tot_ref_frame_types; ++ref_it) {
        MvReferenceFrame ref_pair = pcs_ptr->parent_pcs_ptr->ref_frame_type_arr[ref_it];
        MvReferenceFrame rf[2];
        av1_set_ref_frame(rf, ref_pair);
#endif
        //single ref/list
        if (rf[1] == NONE_FRAME) {
            MvReferenceFrame frame_type = rf[0];
            uint8_t          list_idx = get_list_idx(rf[0]);
            uint8_t          ref_idx = get_ref_frame_idx(rf[0]);
            // Always consider the 2 closet ref frames (i.e. ref_idx=0) @ MVP cand generation
#if CLN_MD_CANDS
            if (!is_valid_unipred_ref(context_ptr, MIN(TOT_INTER_GROUP - 1, NRST_NEAR_GROUP), list_idx, ref_idx)) continue;
#if !FTR_NEW_REF_PRUNING_CTRLS
            if (ref_idx > context_ptr->md_max_ref_count - 1) continue;
#endif
#else
            if (!is_valid_unipred_ref(context_ptr, MIN(TOT_INTER_GROUP - 1, NRST_NEAR_GROUP), list_idx, ref_idx)) return;
            if (ref_idx > context_ptr->md_max_ref_count - 1) return;
#endif
            //NEAREST
            int16_t to_inject_mv_x = context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                .ref_mvs[frame_type][0]
                .as_mv.col;
            int16_t to_inject_mv_y = context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                .ref_mvs[frame_type][0]
                .as_mv.row;

            inj_mv = list_idx == 0
                ? context_ptr->injected_mv_count_l0 == 0 ||
                mrp_is_already_injected_mv_l0(
                    context_ptr, to_inject_mv_x, to_inject_mv_y, frame_type) == EB_FALSE
                : context_ptr->injected_mv_count_l1 == 0 ||
                mrp_is_already_injected_mv_l1(
                    context_ptr, to_inject_mv_x, to_inject_mv_y, frame_type) == EB_FALSE;

            if (umv0tile)
                inside_tile = is_inside_tile_boundary(&(xd->tile),
                    to_inject_mv_x,
                    to_inject_mv_y,
                    mi_col,
                    mi_row,
                    context_ptr->blk_geom->bsize);
            inj_mv = inj_mv && inside_tile;
            if (inj_mv) {
                uint8_t inter_type;
                uint8_t is_ii_allowed = svt_is_interintra_allowed(context_ptr->inter_intra_comp_ctrls.enabled, bsize, NEARESTMV, rf);
#if !FTR_NEW_REF_PRUNING_CTRLS // intra-inter
                if (is_ii_allowed && context_ptr->inter_intra_comp_ctrls.closest_ref_only)
                    if (ref_idx > 0)
                        is_ii_allowed = 0;
#endif
                uint8_t tot_inter_types = is_ii_allowed ? II_COUNT : 1;
                uint8_t is_obmc_allowed =
                    obmc_motion_mode_allowed(pcs_ptr, context_ptr, bsize, rf[0], rf[1], NEARESTMV) ==
                    OBMC_CAUSAL;
#if !FTR_NEW_REF_PRUNING_CTRLS // obmc
                is_obmc_allowed = context_ptr->obmc_ctrls.mvp_ref_count == 0 ? 0 :
                    ref_idx > context_ptr->obmc_ctrls.mvp_ref_count - 1 ? 0 : is_obmc_allowed;
#endif
                tot_inter_types = is_obmc_allowed ? tot_inter_types + 1 : tot_inter_types;
                for (inter_type = 0; inter_type < tot_inter_types; inter_type++) {
#if FTR_NEW_REF_PRUNING_CTRLS
                    if (!is_valid_uni_type(context_ptr, inter_type, is_ii_allowed, 0, list_idx, ref_idx))
                        continue;
#endif
                    cand_array[cand_idx].type = INTER_MODE;
#if !CLN_MD_CANDS
                    cand_array[cand_idx].inter_mode = NEARESTMV;
#endif
                    cand_array[cand_idx].pred_mode = NEARESTMV;
                    cand_array[cand_idx].motion_mode = SIMPLE_TRANSLATION;
                    cand_array[cand_idx].is_compound = 0;
#if !CLN_MD_CANDS
                    cand_array[cand_idx].distortion_ready = 0;
#endif
                    cand_array[cand_idx].use_intrabc = 0;
                    cand_array[cand_idx].skip_mode_allowed = EB_FALSE;
                    cand_array[cand_idx].prediction_direction[0] = list_idx;
#if !CLN_MD_CANDS
                    cand_array[cand_idx].is_new_mv = 0;
#endif
                    cand_array[cand_idx].drl_index = 0;
#if !CLN_MD_CANDS
                    cand_array[cand_idx].ref_mv_index = 0;
#endif
                    cand_array[cand_idx].ref_frame_type = frame_type;
#if !CLN_MD_CANDS
                    cand_array[cand_idx].ref_frame_index_l0 = (list_idx == 0) ? ref_idx : -1;
                    cand_array[cand_idx].ref_frame_index_l1 = (list_idx == 1) ? ref_idx : -1;
                    cand_array[cand_idx].transform_type[0] = DCT_DCT;
                    cand_array[cand_idx].transform_type_uv = DCT_DCT;
#endif
                    if (list_idx == 0) {
                        cand_array[cand_idx].motion_vector_xl0 = to_inject_mv_x;
                        cand_array[cand_idx].motion_vector_yl0 = to_inject_mv_y;
#if !CLN_MD_CANDS
                        context_ptr->injected_mv_x_l0_array[context_ptr->injected_mv_count_l0] =
                            to_inject_mv_x;
                        context_ptr->injected_mv_y_l0_array[context_ptr->injected_mv_count_l0] =
                            to_inject_mv_y;
                        context_ptr->injected_ref_type_l0_array[context_ptr->injected_mv_count_l0] =
                            frame_type;
                        ++context_ptr->injected_mv_count_l0;
#endif
                    }
                    else {
                        cand_array[cand_idx].motion_vector_xl1 = to_inject_mv_x;
                        cand_array[cand_idx].motion_vector_yl1 = to_inject_mv_y;
#if !CLN_MD_CANDS
                        context_ptr->injected_mv_x_l1_array[context_ptr->injected_mv_count_l1] =
                            to_inject_mv_x;
                        context_ptr->injected_mv_y_l1_array[context_ptr->injected_mv_count_l1] =
                            to_inject_mv_y;
                        context_ptr->injected_ref_type_l1_array[context_ptr->injected_mv_count_l1] =
                            frame_type;
                        ++context_ptr->injected_mv_count_l1;
#endif
                    }
                    if (inter_type == 0) {
                        cand_array[cand_idx].is_interintra_used = 0;
                        cand_array[cand_idx].motion_mode = SIMPLE_TRANSLATION;
                    }
                    else {
                        if (is_ii_allowed) {
                            if (inter_type == 1) {
                                inter_intra_search(pcs_ptr, context_ptr, &cand_array[cand_idx]);
                                cand_array[cand_idx].is_interintra_used = 1;
                                cand_array[cand_idx].use_wedge_interintra = 1;
                            }
                            else if (inter_type == 2) {
                                cand_array[cand_idx].is_interintra_used = 1;
                                cand_array[cand_idx].interintra_mode =
                                    cand_array[cand_idx - 1].interintra_mode;
                                cand_array[cand_idx].use_wedge_interintra = 0;
                            }
                        }
                        if (is_obmc_allowed && inter_type == tot_inter_types - 1) {
                            cand_array[cand_idx].is_interintra_used = 0;
                            cand_array[cand_idx].motion_mode = OBMC_CAUSAL;
                        }
                    }

                    INCRMENT_CAND_TOTAL_COUNT(cand_idx);
                }
#if CLN_MD_CANDS
                if (list_idx == 0) {
                    context_ptr->injected_mv_x_l0_array[context_ptr->injected_mv_count_l0] = to_inject_mv_x;
                    context_ptr->injected_mv_y_l0_array[context_ptr->injected_mv_count_l0] = to_inject_mv_y;
                    context_ptr->injected_ref_type_l0_array[context_ptr->injected_mv_count_l0] = frame_type;
                    ++context_ptr->injected_mv_count_l0;
                }
                else {
                    context_ptr->injected_mv_x_l1_array[context_ptr->injected_mv_count_l1] = to_inject_mv_x;
                    context_ptr->injected_mv_y_l1_array[context_ptr->injected_mv_count_l1] = to_inject_mv_y;
                    context_ptr->injected_ref_type_l1_array[context_ptr->injected_mv_count_l1] = frame_type;
                    ++context_ptr->injected_mv_count_l1;
                }
#endif
            }

            //NEAR
            max_drl_index = get_max_drl_index(xd->ref_mv_count[frame_type], NEARMV);
#if TUNE_M9_IFS_SSE_ADAPT_ME_MV_NEAR_WM_TF
#if TUNE_M7_M9
#if CLN_NEAR_CTRLS
            uint8_t cap_max_drl_index = 0;
            if (context_ptr->near_count_ctrls.enabled)
                cap_max_drl_index = MIN(context_ptr->near_count_ctrls.near_count, max_drl_index);
#else
            uint8_t cap_max_drl_index = (pcs_ptr->enc_mode <= ENC_M9) ? max_drl_index : MIN(0, max_drl_index);
#endif
#else
            uint8_t cap_max_drl_index = (pcs_ptr->enc_mode <= ENC_M8) ? max_drl_index : MIN(0, max_drl_index);
#endif
            for (drli = 0; drli < cap_max_drl_index; drli++) {
#else
            for (drli = 0; drli < max_drl_index; drli++) {
#endif
                get_av1_mv_pred_drl(
                    context_ptr, blk_ptr, frame_type, 0, NEARMV, drli, nearestmv, nearmv, ref_mv);

                to_inject_mv_x = nearmv[0].as_mv.col;
                to_inject_mv_y = nearmv[0].as_mv.row;

                inj_mv =
                    list_idx == 0
                    ? context_ptr->injected_mv_count_l0 == 0 ||
                    mrp_is_already_injected_mv_l0(
                        context_ptr, to_inject_mv_x, to_inject_mv_y, frame_type) == EB_FALSE
                    : context_ptr->injected_mv_count_l1 == 0 ||
                    mrp_is_already_injected_mv_l1(
                        context_ptr, to_inject_mv_x, to_inject_mv_y, frame_type) == EB_FALSE;

                if (umv0tile)
                    inside_tile = is_inside_tile_boundary(&(xd->tile),
                        to_inject_mv_x,
                        to_inject_mv_y,
                        mi_col,
                        mi_row,
                        context_ptr->blk_geom->bsize);
                inj_mv = inj_mv && inside_tile;
                if (inj_mv) {
                    uint8_t inter_type;
                    uint8_t is_ii_allowed = svt_is_interintra_allowed(context_ptr->inter_intra_comp_ctrls.enabled, bsize, NEARMV, rf);
#if !FTR_NEW_REF_PRUNING_CTRLS // intra-inter
                    if (is_ii_allowed && context_ptr->inter_intra_comp_ctrls.closest_ref_only)
                        if (ref_idx > 0)
                            is_ii_allowed = 0;
#endif
                    uint8_t tot_inter_types = is_ii_allowed ? II_COUNT : 1;
                    uint8_t is_obmc_allowed =
                        obmc_motion_mode_allowed(pcs_ptr, context_ptr, bsize, rf[0], rf[1], NEARMV) ==
                        OBMC_CAUSAL;
#if !FTR_NEW_REF_PRUNING_CTRLS // obmc
                    is_obmc_allowed = context_ptr->obmc_ctrls.mvp_ref_count == 0 ? 0 :
                        ref_idx > context_ptr->obmc_ctrls.mvp_ref_count - 1 ? 0 : is_obmc_allowed;
                    is_obmc_allowed = context_ptr->obmc_ctrls.near_count == 0 ? 0 :
                        drli > context_ptr->obmc_ctrls.near_count - 1 ? 0 : is_obmc_allowed;
#endif
                    tot_inter_types = is_obmc_allowed ? tot_inter_types + 1 : tot_inter_types;
                    for (inter_type = 0; inter_type < tot_inter_types; inter_type++) {
#if FTR_NEW_REF_PRUNING_CTRLS
                        if (!is_valid_uni_type(context_ptr, inter_type, is_ii_allowed, 0, list_idx, ref_idx))
                            continue;
#endif
                        cand_array[cand_idx].type = INTER_MODE;
#if !CLN_MD_CANDS
                        cand_array[cand_idx].inter_mode = NEARMV;
#endif
                        cand_array[cand_idx].pred_mode = NEARMV;
                        cand_array[cand_idx].motion_mode = SIMPLE_TRANSLATION;
                        cand_array[cand_idx].is_compound = 0;
#if !CLN_MD_CANDS
                        cand_array[cand_idx].distortion_ready = 0;
#endif
                        cand_array[cand_idx].use_intrabc = 0;
                        cand_array[cand_idx].skip_mode_allowed = EB_FALSE;
                        cand_array[cand_idx].prediction_direction[0] = list_idx;
#if !CLN_MD_CANDS
                        cand_array[cand_idx].is_new_mv = 0;
#endif
                        cand_array[cand_idx].drl_index = drli;
#if !CLN_MD_CANDS
                        cand_array[cand_idx].ref_mv_index = 0;
#endif
                        cand_array[cand_idx].ref_frame_type = frame_type;

#if !CLN_MD_CANDS
                        cand_array[cand_idx].ref_frame_index_l0 = (list_idx == 0) ? ref_idx : -1;
                        cand_array[cand_idx].ref_frame_index_l1 = (list_idx == 1) ? ref_idx : -1;
                        cand_array[cand_idx].transform_type[0] = DCT_DCT;
                        cand_array[cand_idx].transform_type_uv = DCT_DCT;
#endif
                        if (list_idx == 0) {
                            cand_array[cand_idx].motion_vector_xl0 = to_inject_mv_x;
                            cand_array[cand_idx].motion_vector_yl0 = to_inject_mv_y;
#if !CLN_MD_CANDS
                            context_ptr->injected_mv_x_l0_array[context_ptr->injected_mv_count_l0] =
                                to_inject_mv_x;
                            context_ptr->injected_mv_y_l0_array[context_ptr->injected_mv_count_l0] =
                                to_inject_mv_y;
                            context_ptr->injected_ref_type_l0_array[context_ptr->injected_mv_count_l0] =
                                frame_type;
                            ++context_ptr->injected_mv_count_l0;
#endif
                        }
                        else {
                            cand_array[cand_idx].motion_vector_xl1 = to_inject_mv_x;
                            cand_array[cand_idx].motion_vector_yl1 = to_inject_mv_y;
#if !CLN_MD_CANDS
                            context_ptr->injected_mv_x_l1_array[context_ptr->injected_mv_count_l1] =
                                to_inject_mv_x;
                            context_ptr->injected_mv_y_l1_array[context_ptr->injected_mv_count_l1] =
                                to_inject_mv_y;
                            context_ptr->injected_ref_type_l1_array[context_ptr->injected_mv_count_l1] =
                                frame_type;
                            ++context_ptr->injected_mv_count_l1;
#endif
                        }
                        if (inter_type == 0) {
                            cand_array[cand_idx].is_interintra_used = 0;
                            cand_array[cand_idx].motion_mode = SIMPLE_TRANSLATION;
                        }
                        else {
                            if (is_ii_allowed) {
                                if (inter_type == 1) {
                                    inter_intra_search(pcs_ptr, context_ptr, &cand_array[cand_idx]);
                                    cand_array[cand_idx].is_interintra_used = 1;
                                    cand_array[cand_idx].use_wedge_interintra = 1;
                                }
                                else if (inter_type == 2) {
                                    cand_array[cand_idx].is_interintra_used = 1;
                                    cand_array[cand_idx].interintra_mode =
                                        cand_array[cand_idx - 1].interintra_mode;
                                    cand_array[cand_idx].use_wedge_interintra = 0;
                                }
                            }
                            if (is_obmc_allowed && inter_type == tot_inter_types - 1) {
                                cand_array[cand_idx].is_interintra_used = 0;
                                cand_array[cand_idx].motion_mode = OBMC_CAUSAL;
                            }
                        }

                        INCRMENT_CAND_TOTAL_COUNT(cand_idx);
                    }
#if CLN_MD_CANDS
                    if (list_idx == 0) {
                        context_ptr->injected_mv_x_l0_array[context_ptr->injected_mv_count_l0] = to_inject_mv_x;
                        context_ptr->injected_mv_y_l0_array[context_ptr->injected_mv_count_l0] = to_inject_mv_y;
                        context_ptr->injected_ref_type_l0_array[context_ptr->injected_mv_count_l0] = frame_type;
                        ++context_ptr->injected_mv_count_l0;
                    }
                    else {
                        context_ptr->injected_mv_x_l1_array[context_ptr->injected_mv_count_l1] = to_inject_mv_x;
                        context_ptr->injected_mv_y_l1_array[context_ptr->injected_mv_count_l1] = to_inject_mv_y;
                        context_ptr->injected_ref_type_l1_array[context_ptr->injected_mv_count_l1] = frame_type;
                        ++context_ptr->injected_mv_count_l1;
                    }
#endif
                }
            }
        }
        else if (allow_compound) {
            uint8_t ref_idx_0 = get_ref_frame_idx(rf[0]);
            uint8_t ref_idx_1 = get_ref_frame_idx(rf[1]);

            uint8_t list_idx_0 = get_list_idx(rf[0]);
            uint8_t list_idx_1 = get_list_idx(rf[1]);
            // Always consider the 2 closet ref frames (i.e. ref_idx=0) @ MVP cand generation
#if CLN_MD_CANDS
            if (!is_valid_bipred_ref(
                context_ptr, NRST_NEAR_GROUP, list_idx_0, ref_idx_0, list_idx_1, ref_idx_1)) continue;
#if !FTR_NEW_REF_PRUNING_CTRLS
            if (ref_idx_0 > context_ptr->md_max_ref_count - 1 ||
                ref_idx_1 > context_ptr->md_max_ref_count - 1)
                continue;
#endif
#else
            if (!is_valid_bipred_ref(
                context_ptr, NRST_NEAR_GROUP, list_idx_0, ref_idx_0, list_idx_1, ref_idx_1)) return;
            if (ref_idx_0 > context_ptr->md_max_ref_count - 1 ||
                ref_idx_1 > context_ptr->md_max_ref_count - 1)
                return;
#endif
            {
                //NEAREST_NEAREST
#if FTR_UPGRADE_COMP_LEVELS
                MD_COMP_TYPE tot_comp_types =
                    (context_ptr->inter_comp_ctrls.do_nearest_nearest == 0)
                    ? MD_COMP_DIST
                    : MD_COMP_TYPES;
#endif
                int16_t to_inject_mv_x_l0 =
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                    .ed_ref_mv_stack[ref_pair][0]
                    .this_mv.as_mv.col;
                int16_t to_inject_mv_y_l0 =
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                    .ed_ref_mv_stack[ref_pair][0]
                    .this_mv.as_mv.row;
                int16_t to_inject_mv_x_l1 =
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                    .ed_ref_mv_stack[ref_pair][0]
                    .comp_mv.as_mv.col;
                int16_t to_inject_mv_y_l1 =
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                    .ed_ref_mv_stack[ref_pair][0]
                    .comp_mv.as_mv.row;

                inj_mv = context_ptr->injected_mv_count_bipred == 0 ||
                    mrp_is_already_injected_mv_bipred(context_ptr,
                        to_inject_mv_x_l0,
                        to_inject_mv_y_l0,
                        to_inject_mv_x_l1,
                        to_inject_mv_y_l1,
                        ref_pair) == EB_FALSE;

                if (umv0tile) {
                    inside_tile = is_inside_tile_boundary(&(xd->tile),
                        to_inject_mv_x_l0,
                        to_inject_mv_y_l0,
                        mi_col,
                        mi_row,
                        context_ptr->blk_geom->bsize) &&
                        is_inside_tile_boundary(&(xd->tile),
                            to_inject_mv_x_l1,
                            to_inject_mv_y_l1,
                            mi_col,
                            mi_row,
                            context_ptr->blk_geom->bsize);
                }
                inj_mv = inj_mv && inside_tile;
                if (inj_mv) {
                    // Set the allowable compound types to be injected
                    uint8_t allowed_comp_types[MD_COMP_TYPES];
                    memcpy(allowed_comp_types, context_ptr->inter_comp_ctrls.allowed_comp_types, sizeof(uint8_t) * MD_COMP_TYPES);
#if !FTR_NEW_REF_PRUNING_CTRLS
                    // Reduce the compound types to inject based on the distance of the reference frames from the current frame
                    set_comp_types_by_distance(allowed_comp_types,
                        context_ptr->inter_comp_ctrls.allowed_dist1_comp_types,
                        context_ptr->inter_comp_ctrls.allowed_dist2_comp_types,
                        ref_idx_0,
                        ref_idx_1);
#endif
                    EbBool is_skip_mode =
                        pcs_ptr->parent_pcs_ptr->is_skip_mode_allowed &&
                        (rf[0] == frm_hdr->skip_mode_params.ref_frame_idx_0 + 1) &&
                        (rf[1] == frm_hdr->skip_mode_params.ref_frame_idx_1 + 1)
                        ? EB_TRUE : EB_FALSE;
                    EbBool mask_done = 0;
#if FTR_UPGRADE_COMP_LEVELS
                    for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < tot_comp_types; cur_type++) {
#else
                    for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < MD_COMP_TYPES; cur_type++) {
#endif

                        // Skip the current compound type if not set to be injected
                        if (is_skip_mode && cur_type == MD_COMP_AVG) {
                            //always inject avg for skip_mode
                        }
                        else if (allowed_comp_types[cur_type] == 0)
                            continue;
#if FTR_NEW_REF_PRUNING_CTRLS
                        if (!is_valid_bi_type(context_ptr, cur_type, list_idx_0, ref_idx_0, list_idx_1, ref_idx_1))
                            continue;
#endif
                        cand_array[cand_idx].type = INTER_MODE;
#if !CLN_MD_CANDS
                        cand_array[cand_idx].inter_mode = NEAREST_NEARESTMV;
#endif
                        cand_array[cand_idx].pred_mode = NEAREST_NEARESTMV;
                        cand_array[cand_idx].motion_mode = SIMPLE_TRANSLATION;
                        cand_array[cand_idx].is_compound = 1;
                        cand_array[cand_idx].is_interintra_used = 0;
#if !CLN_MD_CANDS
                        cand_array[cand_idx].distortion_ready = 0;
#endif
                        cand_array[cand_idx].use_intrabc = 0;
                        cand_array[cand_idx].skip_mode_allowed =
                            cur_type == MD_COMP_AVG && is_skip_mode ? EB_TRUE : EB_FALSE;

                        cand_array[cand_idx].prediction_direction[0] = BI_PRED;
#if !CLN_MD_CANDS
                        cand_array[cand_idx].is_new_mv = 0;
#endif
                        cand_array[cand_idx].motion_vector_xl0 = to_inject_mv_x_l0;
                        cand_array[cand_idx].motion_vector_yl0 = to_inject_mv_y_l0;
                        cand_array[cand_idx].motion_vector_xl1 = to_inject_mv_x_l1;
                        cand_array[cand_idx].motion_vector_yl1 = to_inject_mv_y_l1;
                        cand_array[cand_idx].drl_index = 0;
#if !CLN_MD_CANDS
                        cand_array[cand_idx].ref_mv_index = 0;
#endif
                        cand_array[cand_idx].ref_frame_type = ref_pair;
#if !CLN_MD_CANDS
                        cand_array[cand_idx].ref_frame_index_l0 = ref_idx_0;
                        cand_array[cand_idx].ref_frame_index_l1 = ref_idx_1;
                        cand_array[cand_idx].transform_type[0] = DCT_DCT;
                        cand_array[cand_idx].transform_type_uv = DCT_DCT;
#endif
#if !FTR_UPGRADE_COMP_LEVELS
                        context_ptr
                            ->injected_mv_x_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_x_l0;
                        context_ptr
                            ->injected_mv_y_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_y_l0;
                        context_ptr
                            ->injected_mv_x_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_x_l1;
                        context_ptr
                            ->injected_mv_y_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_y_l1;
                        context_ptr
                            ->injected_ref_type_bipred_array[context_ptr->injected_mv_count_bipred] =
                            ref_pair;
                        ++context_ptr->injected_mv_count_bipred;
#endif
                        //NRST-NRST
#if FTR_UPGRADE_COMP_LEVELS
                        if (cur_type > MD_COMP_AVG) {
                            if (mask_done != 1) {
                                if (calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_idx]))
                                    break;
                                mask_done = 1;
                            }
                        }
#else
                        if (cur_type == MD_COMP_DIFF0 || cur_type == MD_COMP_WEDGE) {
                            if (mask_done != 1) {
                                calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_idx]);
                                mask_done = 1;
                            }
                        }
#endif
                        determine_compound_mode(pcs_ptr, context_ptr, &cand_array[cand_idx], cur_type);
                        INCRMENT_CAND_TOTAL_COUNT(cand_idx);
                    }
#if FTR_UPGRADE_COMP_LEVELS
                    context_ptr
                        ->injected_mv_x_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                        to_inject_mv_x_l0;
                    context_ptr
                        ->injected_mv_y_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                        to_inject_mv_y_l0;
                    context_ptr
                        ->injected_mv_x_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                        to_inject_mv_x_l1;
                    context_ptr
                        ->injected_mv_y_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                        to_inject_mv_y_l1;
                    context_ptr
                        ->injected_ref_type_bipred_array[context_ptr->injected_mv_count_bipred] =
                        ref_pair;
                    ++context_ptr->injected_mv_count_bipred;
#endif
                }

                //NEAR_NEAR
#if FTR_UPGRADE_COMP_LEVELS
                tot_comp_types =
                    (context_ptr->inter_comp_ctrls.do_near_near == 0)
                    ? MD_COMP_DIST
                    : MD_COMP_TYPES;
#endif
                max_drl_index = get_max_drl_index(xd->ref_mv_count[ref_pair], NEAR_NEARMV);
#if CLN_NEAR_CTRLS
                uint8_t cap_max_drl_index = 0;
                if (context_ptr->near_count_ctrls.enabled)
                    cap_max_drl_index = MIN(context_ptr->near_count_ctrls.near_near_count, max_drl_index);

                for (drli = 0; drli < cap_max_drl_index; drli++) {
#else
                for (drli = 0; drli < max_drl_index; drli++) {
#endif
                    get_av1_mv_pred_drl(context_ptr,
                        blk_ptr,
                        ref_pair,
                        1,
                        NEAR_NEARMV,
                        drli,
                        nearestmv,
                        nearmv,
                        ref_mv);

                    to_inject_mv_x_l0 = nearmv[0].as_mv.col;
                    to_inject_mv_y_l0 = nearmv[0].as_mv.row;
                    to_inject_mv_x_l1 = nearmv[1].as_mv.col;
                    to_inject_mv_y_l1 = nearmv[1].as_mv.row;

                    inj_mv = context_ptr->injected_mv_count_bipred == 0 ||
                        mrp_is_already_injected_mv_bipred(context_ptr,
                            to_inject_mv_x_l0,
                            to_inject_mv_y_l0,
                            to_inject_mv_x_l1,
                            to_inject_mv_y_l1,
                            ref_pair) == EB_FALSE;

                    if (umv0tile) {
                        inside_tile = is_inside_tile_boundary(&(xd->tile),
                            to_inject_mv_x_l0,
                            to_inject_mv_y_l0,
                            mi_col,
                            mi_row,
                            context_ptr->blk_geom->bsize) &&
                            is_inside_tile_boundary(&(xd->tile),
                                to_inject_mv_x_l1,
                                to_inject_mv_y_l1,
                                mi_col,
                                mi_row,
                                context_ptr->blk_geom->bsize);
                    }
                    inj_mv = inj_mv && inside_tile;
                    if (inj_mv) {
                        // Set the allowable compound types to be injected
                        uint8_t allowed_comp_types[MD_COMP_TYPES];
                        memcpy(allowed_comp_types, context_ptr->inter_comp_ctrls.allowed_comp_types, sizeof(uint8_t) * MD_COMP_TYPES);
#if !FTR_NEW_REF_PRUNING_CTRLS
                        // Reduce the compound types to inject based on the distance of the reference frames from the current frame
                        set_comp_types_by_distance(allowed_comp_types,
                            context_ptr->inter_comp_ctrls.allowed_dist1_comp_types,
                            context_ptr->inter_comp_ctrls.allowed_dist2_comp_types,
                            ref_idx_0,
                            ref_idx_1);
#endif
                        EbBool mask_done = 0;
#if FTR_UPGRADE_COMP_LEVELS
                        for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < tot_comp_types; cur_type++) {
#else
                        for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < MD_COMP_TYPES; cur_type++) {
#endif

                            // Skip the current compound type if not set to be injected
                            if (allowed_comp_types[cur_type] == 0)
                                continue;
#if FTR_NEW_REF_PRUNING_CTRLS
                            if (!is_valid_bi_type(context_ptr, cur_type, list_idx_0, ref_idx_0, list_idx_1, ref_idx_1))
                                continue;
#endif
                            cand_array[cand_idx].type = INTER_MODE;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].inter_mode = NEAR_NEARMV;
#endif
                            cand_array[cand_idx].pred_mode = NEAR_NEARMV;
                            cand_array[cand_idx].motion_mode = SIMPLE_TRANSLATION;
                            cand_array[cand_idx].is_compound = 1;
                            cand_array[cand_idx].is_interintra_used = 0;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].distortion_ready = 0;
#endif
                            cand_array[cand_idx].use_intrabc = 0;
                            cand_array[cand_idx].skip_mode_allowed = EB_FALSE;
                            cand_array[cand_idx].prediction_direction[0] = BI_PRED;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].is_new_mv = 0;
#endif
                            cand_array[cand_idx].motion_vector_xl0 = to_inject_mv_x_l0;
                            cand_array[cand_idx].motion_vector_yl0 = to_inject_mv_y_l0;
                            cand_array[cand_idx].motion_vector_xl1 = to_inject_mv_x_l1;
                            cand_array[cand_idx].motion_vector_yl1 = to_inject_mv_y_l1;

                            cand_array[cand_idx].drl_index = drli;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].ref_mv_index = 0;
#endif
                            cand_array[cand_idx].ref_frame_type = ref_pair;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].ref_frame_index_l0 = ref_idx_0;
                            cand_array[cand_idx].ref_frame_index_l1 = ref_idx_1;
                            cand_array[cand_idx].transform_type[0] = DCT_DCT;
                            cand_array[cand_idx].transform_type_uv = DCT_DCT;
#endif
#if FTR_UPGRADE_COMP_LEVELS
                            if (cur_type > MD_COMP_AVG) {
                                if (mask_done != 1) {
                                    if (calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_idx]))
                                        break;
                                    mask_done = 1;
                                }
                            }
#else
                            context_ptr
                                ->injected_mv_x_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                                to_inject_mv_x_l0;
                            context_ptr
                                ->injected_mv_y_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                                to_inject_mv_y_l0;
                            context_ptr
                                ->injected_mv_x_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                                to_inject_mv_x_l1;
                            context_ptr
                                ->injected_mv_y_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                                to_inject_mv_y_l1;
                            context_ptr->injected_ref_type_bipred_array
                                [context_ptr->injected_mv_count_bipred] = ref_pair;
                            ++context_ptr->injected_mv_count_bipred;

                            //NR-NR
                            if (cur_type == MD_COMP_DIFF0 || cur_type == MD_COMP_WEDGE) {
                                if (mask_done != 1) {
                                    calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_idx]);
                                    mask_done = 1;
                                }
                            }
#endif
                            determine_compound_mode(
                                pcs_ptr, context_ptr, &cand_array[cand_idx], cur_type);
                            INCRMENT_CAND_TOTAL_COUNT(cand_idx);
                        }
#if FTR_UPGRADE_COMP_LEVELS
                        context_ptr
                            ->injected_mv_x_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_x_l0;
                        context_ptr
                            ->injected_mv_y_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_y_l0;
                        context_ptr
                            ->injected_mv_x_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_x_l1;
                        context_ptr
                            ->injected_mv_y_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_y_l1;
                        context_ptr->injected_ref_type_bipred_array
                            [context_ptr->injected_mv_count_bipred] = ref_pair;
                        ++context_ptr->injected_mv_count_bipred;
#endif
                    }
                }
            }
        }
#if CLN_MD_CANDS
    }
#endif
    //update tot Candidate count
    *candTotCnt = cand_idx;
}

#if CLN_MD_CANDS
void inject_new_nearest_new_comb_candidates(const SequenceControlSet *  scs_ptr,
                                            PictureControlSet *pcs_ptr,
                                            ModeDecisionContext *context_ptr,
                                            uint32_t *candTotCnt) {
#else
void inject_new_nearest_new_comb_candidates(const SequenceControlSet *  scs_ptr,
                                            struct ModeDecisionContext *context_ptr,
                                            PictureControlSet *pcs_ptr, MvReferenceFrame ref_pair,
                                            uint32_t *candTotCnt) {
#endif
    uint32_t               cand_idx   = *candTotCnt;
    ModeDecisionCandidate *cand_array = context_ptr->fast_candidate_array;
    MacroBlockD *          xd         = context_ptr->blk_ptr->av1xd;
    IntMv                  nearestmv[2], nearmv[2], ref_mv[2];
    int                    umv0tile    = (scs_ptr->static_config.unrestricted_motion_vector == 0);
    uint32_t               mi_row      = context_ptr->blk_origin_y >> MI_SIZE_LOG2;
    uint32_t               mi_col      = context_ptr->blk_origin_x >> MI_SIZE_LOG2;

#if FTR_UPGRADE_COMP_LEVELS
    MD_COMP_TYPE tot_comp_types =
        (context_ptr->inter_comp_ctrls.do_nearest_near_new == 0)
        ? MD_COMP_DIST
        : MD_COMP_TYPES;
#endif
#if CLN_MD_CANDS
    //all of ref pairs: (1)single-ref List0  (2)single-ref List1  (3)compound Bi-Dir List0-List1  (4)compound Uni-Dir List0-List0  (5)compound Uni-Dir List1-List1
    for (uint32_t ref_it = 0; ref_it < pcs_ptr->parent_pcs_ptr->tot_ref_frame_types; ++ref_it) {
        const MvReferenceFrame ref_pair = pcs_ptr->parent_pcs_ptr->ref_frame_type_arr[ref_it];
#endif
        MvReferenceFrame rf[2];
        av1_set_ref_frame(rf, ref_pair);
        {
            uint8_t ref_idx_0 = get_ref_frame_idx(rf[0]);
            uint8_t ref_idx_1 = get_ref_frame_idx(rf[1]);
            uint8_t list_idx_0 = get_list_idx(rf[0]);
            uint8_t list_idx_1 = get_list_idx(rf[1]);
#if CLN_MD_CANDS
            if (list_idx_0 != INVALID_REF)
                if (!is_valid_unipred_ref(context_ptr, MIN(TOT_INTER_GROUP - 1, NRST_NEW_NEAR_GROUP), list_idx_0, ref_idx_0)) continue;
            if (list_idx_1 != INVALID_REF)
                if (!is_valid_unipred_ref(context_ptr, MIN(TOT_INTER_GROUP - 1, NRST_NEW_NEAR_GROUP), list_idx_1, ref_idx_1)) continue;
#if !FTR_NEW_REF_PRUNING_CTRLS
            if (ref_idx_0 > context_ptr->md_max_ref_count - 1 ||
                ref_idx_1 > context_ptr->md_max_ref_count - 1)
                continue;
#endif
#else
            if (list_idx_0 != INVALID_REF)
                if (!is_valid_unipred_ref(context_ptr, MIN(TOT_INTER_GROUP - 1, NRST_NEW_NEAR_GROUP), list_idx_0, ref_idx_0)) return;
            if (list_idx_1 != INVALID_REF)
                if (!is_valid_unipred_ref(context_ptr, MIN(TOT_INTER_GROUP - 1, NRST_NEW_NEAR_GROUP), list_idx_1, ref_idx_1)) return;
            if (ref_idx_0 > context_ptr->md_max_ref_count - 1 ||
                ref_idx_1 > context_ptr->md_max_ref_count - 1)
                return;
#endif
            if (rf[1] != NONE_FRAME) {
                {
                    //NEAREST_NEWMV
                    const MeSbResults *me_results =
                        pcs_ptr->parent_pcs_ptr->pa_me_data->me_results[context_ptr->me_sb_addr];

                    int16_t to_inject_mv_x_l0 =
                        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .ed_ref_mv_stack[ref_pair][0]
                        .this_mv.as_mv.col;
                    int16_t to_inject_mv_y_l0 =
                        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .ed_ref_mv_stack[ref_pair][0]
                        .this_mv.as_mv.row;
                    int16_t to_inject_mv_x_l1 =
                        context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds][get_list_idx(rf[1])]
                        [ref_idx_1][0];
                    int16_t to_inject_mv_y_l1 =
                        context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds][get_list_idx(rf[1])]
                        [ref_idx_1][1];
                    uint8_t inj_mv = context_ptr->injected_mv_count_bipred == 0 ||
                        mrp_is_already_injected_mv_bipred(context_ptr,
                            to_inject_mv_x_l0,
                            to_inject_mv_y_l0,
                            to_inject_mv_x_l1,
                            to_inject_mv_y_l1,
                            ref_pair) == EB_FALSE;

                    int inside_tile = umv0tile ? is_inside_tile_boundary(&(xd->tile),
                        to_inject_mv_x_l0,
                        to_inject_mv_y_l0,
                        mi_col,
                        mi_row,
                        context_ptr->blk_geom->bsize) &&
                        is_inside_tile_boundary(&(xd->tile),
                            to_inject_mv_x_l1,
                            to_inject_mv_y_l1,
                            mi_col,
                            mi_row,
                            context_ptr->blk_geom->bsize) : 1;
                    inj_mv = inj_mv && inside_tile;
                    inj_mv = inj_mv && is_me_data_present(context_ptr, me_results, get_list_idx(rf[1]), ref_idx_1);
                    if (inj_mv) {
                        // Set the allowable compound types to be injected
                        uint8_t allowed_comp_types[MD_COMP_TYPES];
                        memcpy(allowed_comp_types, context_ptr->inter_comp_ctrls.allowed_comp_types, sizeof(uint8_t) * MD_COMP_TYPES);
#if !FTR_NEW_REF_PRUNING_CTRLS
                        // Reduce the compound types to inject based on the distance of the reference frames from the current frame
                        set_comp_types_by_distance(allowed_comp_types,
                            context_ptr->inter_comp_ctrls.allowed_dist1_comp_types,
                            context_ptr->inter_comp_ctrls.allowed_dist2_comp_types,
                            ref_idx_0,
                            ref_idx_1);
#endif
                        EbBool mask_done = 0;
#if FTR_UPGRADE_COMP_LEVELS
                        for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < tot_comp_types; cur_type++) {
#else
                        for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < MD_COMP_TYPES; cur_type++) {
#endif

                            // Skip the current compound type if not set to be injected
                            if (allowed_comp_types[cur_type] == 0)
                                continue;
#if FTR_NEW_REF_PRUNING_CTRLS
                            if (!is_valid_bi_type(context_ptr, cur_type, list_idx_0, ref_idx_0, list_idx_1, ref_idx_1))
                                continue;
#endif
                            cand_array[cand_idx].type = INTER_MODE;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].inter_mode = NEAREST_NEWMV;
#endif
                            cand_array[cand_idx].pred_mode = NEAREST_NEWMV;
                            cand_array[cand_idx].motion_mode = SIMPLE_TRANSLATION;
                            cand_array[cand_idx].is_compound = 1;
                            cand_array[cand_idx].is_interintra_used = 0;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].distortion_ready = 0;
#endif
                            cand_array[cand_idx].use_intrabc = 0;

                            cand_array[cand_idx].skip_mode_allowed = EB_FALSE;

                            cand_array[cand_idx].prediction_direction[0] = BI_PRED;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].is_new_mv = 0;
#endif
                            cand_array[cand_idx].motion_vector_xl0 = to_inject_mv_x_l0;
                            cand_array[cand_idx].motion_vector_yl0 = to_inject_mv_y_l0;
                            cand_array[cand_idx].motion_vector_xl1 = to_inject_mv_x_l1;
                            cand_array[cand_idx].motion_vector_yl1 = to_inject_mv_y_l1;
                            cand_array[cand_idx].drl_index = 0;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].ref_mv_index = 0;
#endif
                            cand_array[cand_idx].ref_frame_type = ref_pair;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].ref_frame_index_l0 = ref_idx_0;
                            cand_array[cand_idx].ref_frame_index_l1 = ref_idx_1;
                            cand_array[cand_idx].transform_type[0] = DCT_DCT;
                            cand_array[cand_idx].transform_type_uv = DCT_DCT;
#endif
                            get_av1_mv_pred_drl(context_ptr,
                                context_ptr->blk_ptr,
                                cand_array[cand_idx].ref_frame_type,
                                cand_array[cand_idx].is_compound,
                                NEAREST_NEWMV,
                                0, //not needed drli,
                                nearestmv,
                                nearmv,
                                ref_mv);
                            cand_array[cand_idx].motion_vector_pred_x[REF_LIST_1] = ref_mv[1].as_mv.col;
                            cand_array[cand_idx].motion_vector_pred_y[REF_LIST_1] = ref_mv[1].as_mv.row;
#if FTR_UPGRADE_COMP_LEVELS
                            //NRST_N
                            if (cur_type > MD_COMP_AVG) {
                                if (mask_done != 1) {
                                    if (calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_idx]))
                                        break;
                                    mask_done = 1;
                                }
                            }
#else
                            context_ptr
                                ->injected_mv_x_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                                to_inject_mv_x_l0;
                            context_ptr
                                ->injected_mv_y_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                                to_inject_mv_y_l0;
                            context_ptr
                                ->injected_mv_x_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                                to_inject_mv_x_l1;
                            context_ptr
                                ->injected_mv_y_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                                to_inject_mv_y_l1;
                            context_ptr->injected_ref_type_bipred_array
                                [context_ptr->injected_mv_count_bipred] = ref_pair;
                            ++context_ptr->injected_mv_count_bipred;
                            //NRST_N
                            if (cur_type == MD_COMP_DIFF0 || cur_type == MD_COMP_WEDGE) {
                                if (mask_done != 1) {
                                    calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_idx]);
                                    mask_done = 1;
                                }
                            }
#endif
                            determine_compound_mode(
                                pcs_ptr, context_ptr, &cand_array[cand_idx], cur_type);
                            INCRMENT_CAND_TOTAL_COUNT(cand_idx);
                        }
#if FTR_UPGRADE_COMP_LEVELS
                        context_ptr
                            ->injected_mv_x_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_x_l0;
                        context_ptr
                            ->injected_mv_y_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_y_l0;
                        context_ptr
                            ->injected_mv_x_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_x_l1;
                        context_ptr
                            ->injected_mv_y_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_y_l1;
                        context_ptr->injected_ref_type_bipred_array
                            [context_ptr->injected_mv_count_bipred] = ref_pair;
                        ++context_ptr->injected_mv_count_bipred;
#endif
                    }
                }

                {
                    //NEW_NEARESTMV
                    const MeSbResults *me_results =
                        pcs_ptr->parent_pcs_ptr->pa_me_data->me_results[context_ptr->me_sb_addr];
                    int16_t to_inject_mv_x_l0 =
                        context_ptr
                        ->sb_me_mv[context_ptr->blk_geom->blkidx_mds][REF_LIST_0][ref_idx_0][0];
                    int16_t to_inject_mv_y_l0 =
                        context_ptr
                        ->sb_me_mv[context_ptr->blk_geom->blkidx_mds][REF_LIST_0][ref_idx_0][1];
                    int16_t to_inject_mv_x_l1 =
                        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .ed_ref_mv_stack[ref_pair][0]
                        .comp_mv.as_mv.col;
                    int16_t to_inject_mv_y_l1 =
                        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .ed_ref_mv_stack[ref_pair][0]
                        .comp_mv.as_mv.row;

                    uint8_t inj_mv = context_ptr->injected_mv_count_bipred == 0 ||
                        mrp_is_already_injected_mv_bipred(context_ptr,
                            to_inject_mv_x_l0,
                            to_inject_mv_y_l0,
                            to_inject_mv_x_l1,
                            to_inject_mv_y_l1,
                            ref_pair) == EB_FALSE;

                    int inside_tile = umv0tile ? is_inside_tile_boundary(&(xd->tile),
                        to_inject_mv_x_l0,
                        to_inject_mv_y_l0,
                        mi_col,
                        mi_row,
                        context_ptr->blk_geom->bsize) &&
                        is_inside_tile_boundary(&(xd->tile),
                            to_inject_mv_x_l1,
                            to_inject_mv_y_l1,
                            mi_col,
                            mi_row,
                            context_ptr->blk_geom->bsize) : 1;
                    inj_mv = inj_mv && inside_tile;
                    inj_mv = inj_mv && is_me_data_present(context_ptr, me_results, 0, ref_idx_0);
                    if (inj_mv) {
                        // Set the allowable compound types to be injected
                        uint8_t allowed_comp_types[MD_COMP_TYPES];
                        memcpy(allowed_comp_types, context_ptr->inter_comp_ctrls.allowed_comp_types, sizeof(uint8_t) * MD_COMP_TYPES);
#if !FTR_NEW_REF_PRUNING_CTRLS
                        // Reduce the compound types to inject based on the distance of the reference frames from the current frame
                        set_comp_types_by_distance(allowed_comp_types,
                            context_ptr->inter_comp_ctrls.allowed_dist1_comp_types,
                            context_ptr->inter_comp_ctrls.allowed_dist2_comp_types,
                            ref_idx_0,
                            ref_idx_1);
#endif
                        EbBool mask_done = 0;
#if FTR_UPGRADE_COMP_LEVELS
                        for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < tot_comp_types; cur_type++) {
#else
                        for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < MD_COMP_TYPES; cur_type++) {
#endif

                            // Skip the current compound type if not set to be injected
                            if (allowed_comp_types[cur_type] == 0)
                                continue;
#if FTR_NEW_REF_PRUNING_CTRLS
                            if (!is_valid_bi_type(context_ptr, cur_type, list_idx_0, ref_idx_0, list_idx_1, ref_idx_1))
                                continue;
#endif
                            cand_array[cand_idx].type = INTER_MODE;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].inter_mode = NEW_NEARESTMV;
#endif
                            cand_array[cand_idx].pred_mode = NEW_NEARESTMV;
                            cand_array[cand_idx].motion_mode = SIMPLE_TRANSLATION;
                            cand_array[cand_idx].is_compound = 1;
                            cand_array[cand_idx].is_interintra_used = 0;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].distortion_ready = 0;
#endif
                            cand_array[cand_idx].use_intrabc = 0;
                            cand_array[cand_idx].skip_mode_allowed = EB_FALSE;
                            cand_array[cand_idx].prediction_direction[0] = BI_PRED;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].is_new_mv = 0;
#endif
                            cand_array[cand_idx].motion_vector_xl0 = to_inject_mv_x_l0;
                            cand_array[cand_idx].motion_vector_yl0 = to_inject_mv_y_l0;
                            cand_array[cand_idx].motion_vector_xl1 = to_inject_mv_x_l1;
                            cand_array[cand_idx].motion_vector_yl1 = to_inject_mv_y_l1;
                            cand_array[cand_idx].drl_index = 0;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].ref_mv_index = 0;
#endif
                            cand_array[cand_idx].ref_frame_type = ref_pair;
#if !CLN_MD_CANDS
                            cand_array[cand_idx].ref_frame_index_l0 = ref_idx_0;
                            cand_array[cand_idx].ref_frame_index_l1 = ref_idx_1;
                            cand_array[cand_idx].transform_type[0] = DCT_DCT;
                            cand_array[cand_idx].transform_type_uv = DCT_DCT;
#endif
                            get_av1_mv_pred_drl(context_ptr,
                                context_ptr->blk_ptr,
                                cand_array[cand_idx].ref_frame_type,
                                cand_array[cand_idx].is_compound,
                                NEW_NEARESTMV,
                                0, //not needed drli,
                                nearestmv,
                                nearmv,
                                ref_mv);
                            cand_array[cand_idx].motion_vector_pred_x[REF_LIST_0] = ref_mv[0].as_mv.col;
                            cand_array[cand_idx].motion_vector_pred_y[REF_LIST_0] = ref_mv[0].as_mv.row;
#if FTR_UPGRADE_COMP_LEVELS
                            if (cur_type > MD_COMP_AVG) {
                                if (mask_done != 1) {
                                    if (calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_idx]))
                                        break;
                                    mask_done = 1;
                                }
                            }
#else
                            context_ptr
                                ->injected_mv_x_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                                to_inject_mv_x_l0;
                            context_ptr
                                ->injected_mv_y_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                                to_inject_mv_y_l0;
                            context_ptr
                                ->injected_mv_x_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                                to_inject_mv_x_l1;
                            context_ptr
                                ->injected_mv_y_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                                to_inject_mv_y_l1;
                            context_ptr->injected_ref_type_bipred_array
                                [context_ptr->injected_mv_count_bipred] = ref_pair;
                            ++context_ptr->injected_mv_count_bipred;
                            //N_NRST
                            if (cur_type == MD_COMP_DIFF0 || cur_type == MD_COMP_WEDGE) {
                                if (mask_done != 1) {
                                    calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_idx]);
                                    mask_done = 1;
                                }
                            }
#endif
                            determine_compound_mode(
                                pcs_ptr, context_ptr, &cand_array[cand_idx], cur_type);
                            INCRMENT_CAND_TOTAL_COUNT(cand_idx);
                        }
#if FTR_UPGRADE_COMP_LEVELS
                        context_ptr
                            ->injected_mv_x_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_x_l0;
                        context_ptr
                            ->injected_mv_y_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_y_l0;
                        context_ptr
                            ->injected_mv_x_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_x_l1;
                        context_ptr
                            ->injected_mv_y_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_y_l1;
                        context_ptr->injected_ref_type_bipred_array
                            [context_ptr->injected_mv_count_bipred] = ref_pair;
                        ++context_ptr->injected_mv_count_bipred;
#endif
                    }
                }
                //NEW_NEARMV
                {
                    uint8_t max_drl_index = get_max_drl_index(xd->ref_mv_count[ref_pair], NEW_NEARMV);

                    for (uint8_t drli = 0; drli < max_drl_index; drli++) {
                        get_av1_mv_pred_drl(context_ptr,
                            context_ptr->blk_ptr,
                            ref_pair,
                            1,
                            NEW_NEARMV,
                            drli,
                            nearestmv,
                            nearmv,
                            ref_mv);

                        //NEW_NEARMV
                        const MeSbResults *me_results =
                            pcs_ptr->parent_pcs_ptr->pa_me_data->me_results[context_ptr->me_sb_addr];
                        int16_t to_inject_mv_x_l0 =
                            context_ptr
                            ->sb_me_mv[context_ptr->blk_geom->blkidx_mds][REF_LIST_0][ref_idx_0][0];
                        int16_t to_inject_mv_y_l0 =
                            context_ptr
                            ->sb_me_mv[context_ptr->blk_geom->blkidx_mds][REF_LIST_0][ref_idx_0][1];
                        int16_t to_inject_mv_x_l1 = nearmv[1].as_mv.col;
                        int16_t to_inject_mv_y_l1 = nearmv[1].as_mv.row;

                        uint8_t inj_mv = context_ptr->injected_mv_count_bipred == 0 ||
                            mrp_is_already_injected_mv_bipred(context_ptr,
                                to_inject_mv_x_l0,
                                to_inject_mv_y_l0,
                                to_inject_mv_x_l1,
                                to_inject_mv_y_l1,
                                ref_pair) == EB_FALSE;
                        inj_mv = inj_mv && is_me_data_present(context_ptr, me_results, 0, ref_idx_0);
                        if (inj_mv) {
                            // Set the allowable compound types to be injected
                            uint8_t allowed_comp_types[MD_COMP_TYPES];
                            memcpy(allowed_comp_types, context_ptr->inter_comp_ctrls.allowed_comp_types, sizeof(uint8_t) * MD_COMP_TYPES);
#if !FTR_NEW_REF_PRUNING_CTRLS
                            // Reduce the compound types to inject based on the distance of the reference frames from the current frame
                            set_comp_types_by_distance(allowed_comp_types,
                                context_ptr->inter_comp_ctrls.allowed_dist1_comp_types,
                                context_ptr->inter_comp_ctrls.allowed_dist2_comp_types,
                                ref_idx_0,
                                ref_idx_1);
#endif
                            EbBool mask_done = 0;
#if FTR_UPGRADE_COMP_LEVELS
                            for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < tot_comp_types; cur_type++) {
#else
                            for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < MD_COMP_TYPES; cur_type++) {
#endif

                                // Skip the current compound type if not set to be injected
                                if (allowed_comp_types[cur_type] == 0)
                                    continue;
#if FTR_NEW_REF_PRUNING_CTRLS
                                if (!is_valid_bi_type(context_ptr, cur_type, list_idx_0, ref_idx_0, list_idx_1, ref_idx_1))
                                    continue;
#endif
                                cand_array[cand_idx].type = INTER_MODE;
#if !CLN_MD_CANDS
                                cand_array[cand_idx].inter_mode = NEW_NEARMV;
#endif
                                cand_array[cand_idx].pred_mode = NEW_NEARMV;
                                cand_array[cand_idx].motion_mode = SIMPLE_TRANSLATION;
                                cand_array[cand_idx].is_compound = 1;
                                cand_array[cand_idx].is_interintra_used = 0;
#if !CLN_MD_CANDS
                                cand_array[cand_idx].distortion_ready = 0;
#endif
                                cand_array[cand_idx].use_intrabc = 0;
                                cand_array[cand_idx].skip_mode_allowed = EB_FALSE;

                                cand_array[cand_idx].prediction_direction[0] = BI_PRED;
#if !CLN_MD_CANDS
                                cand_array[cand_idx].is_new_mv = 0;
#endif
                                cand_array[cand_idx].motion_vector_xl0 = to_inject_mv_x_l0;
                                cand_array[cand_idx].motion_vector_yl0 = to_inject_mv_y_l0;
                                cand_array[cand_idx].motion_vector_xl1 = to_inject_mv_x_l1;
                                cand_array[cand_idx].motion_vector_yl1 = to_inject_mv_y_l1;

                                cand_array[cand_idx].drl_index = drli;
#if !CLN_MD_CANDS
                                cand_array[cand_idx].ref_mv_index = 0;
#endif
                                cand_array[cand_idx].ref_frame_type = ref_pair;
#if !CLN_MD_CANDS
                                cand_array[cand_idx].ref_frame_index_l0 = ref_idx_0;
                                cand_array[cand_idx].ref_frame_index_l1 = ref_idx_1;
                                cand_array[cand_idx].transform_type[0] = DCT_DCT;
                                cand_array[cand_idx].transform_type_uv = DCT_DCT;
#endif
                                cand_array[cand_idx].motion_vector_pred_x[REF_LIST_0] =
                                    ref_mv[0].as_mv.col;
                                cand_array[cand_idx].motion_vector_pred_y[REF_LIST_0] =
                                    ref_mv[0].as_mv.row;

#if FTR_UPGRADE_COMP_LEVELS
                                if (cur_type > MD_COMP_AVG) {
                                    if (mask_done != 1) {
                                        if (calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_idx]))
                                            break;
                                        mask_done = 1;
                                    }
                                }
#else
                                context_ptr->injected_mv_x_bipred_l0_array
                                    [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l0;
                                context_ptr->injected_mv_y_bipred_l0_array
                                    [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l0;
                                context_ptr->injected_mv_x_bipred_l1_array
                                    [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l1;
                                context_ptr->injected_mv_y_bipred_l1_array
                                    [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l1;
                                context_ptr->injected_ref_type_bipred_array
                                    [context_ptr->injected_mv_count_bipred] = ref_pair;
                                ++context_ptr->injected_mv_count_bipred;

                                //NEW_NEARMV
                                if (cur_type == MD_COMP_DIFF0 || cur_type == MD_COMP_WEDGE) {
                                    if (mask_done != 1) {
                                        calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_idx]);
                                        mask_done = 1;
                                    }
                                }
#endif
                                determine_compound_mode(
                                    pcs_ptr, context_ptr, &cand_array[cand_idx], cur_type);

                                INCRMENT_CAND_TOTAL_COUNT(cand_idx);
                            }
#if FTR_UPGRADE_COMP_LEVELS
                        context_ptr->injected_mv_x_bipred_l0_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l0;
                        context_ptr->injected_mv_y_bipred_l0_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l0;
                        context_ptr->injected_mv_x_bipred_l1_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l1;
                        context_ptr->injected_mv_y_bipred_l1_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l1;
                        context_ptr->injected_ref_type_bipred_array
                            [context_ptr->injected_mv_count_bipred] = ref_pair;
                        ++context_ptr->injected_mv_count_bipred;
#endif
                        }
                    }
                }
                //NEAR_NEWMV
                {
                    uint8_t max_drl_index = get_max_drl_index(xd->ref_mv_count[ref_pair], NEAR_NEWMV);

                    for (uint8_t drli = 0; drli < max_drl_index; drli++) {
                        get_av1_mv_pred_drl(context_ptr,
                            context_ptr->blk_ptr,
                            ref_pair,
                            1,
                            NEAR_NEWMV,
                            drli,
                            nearestmv,
                            nearmv,
                            ref_mv);

                        //NEAR_NEWMV
                        const MeSbResults *me_results =
                            pcs_ptr->parent_pcs_ptr->pa_me_data->me_results[context_ptr->me_sb_addr];

                        int16_t to_inject_mv_x_l0 = nearmv[0].as_mv.col;
                        int16_t to_inject_mv_y_l0 = nearmv[0].as_mv.row;
                        int16_t to_inject_mv_x_l1 =
                            context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                            [get_list_idx(rf[1])][ref_idx_1][0];
                        int16_t to_inject_mv_y_l1 =
                            context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                            [get_list_idx(rf[1])][ref_idx_1][1];
                        uint8_t inj_mv = context_ptr->injected_mv_count_bipred == 0 ||
                            mrp_is_already_injected_mv_bipred(context_ptr,
                                to_inject_mv_x_l0,
                                to_inject_mv_y_l0,
                                to_inject_mv_x_l1,
                                to_inject_mv_y_l1,
                                ref_pair) == EB_FALSE;
                        inj_mv = inj_mv && is_me_data_present(context_ptr, me_results, get_list_idx(rf[1]), ref_idx_1);
                        if (inj_mv) {
                            // Set the allowable compound types to be injected
                            uint8_t allowed_comp_types[MD_COMP_TYPES];
                            memcpy(allowed_comp_types, context_ptr->inter_comp_ctrls.allowed_comp_types, sizeof(uint8_t) * MD_COMP_TYPES);
#if !FTR_NEW_REF_PRUNING_CTRLS
                            // Reduce the compound types to inject based on the distance of the reference frames from the current frame
                            set_comp_types_by_distance(allowed_comp_types,
                                context_ptr->inter_comp_ctrls.allowed_dist1_comp_types,
                                context_ptr->inter_comp_ctrls.allowed_dist2_comp_types,
                                ref_idx_0,
                                ref_idx_1);
#endif
                            EbBool mask_done = 0;
#if FTR_UPGRADE_COMP_LEVELS
                            for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < tot_comp_types; cur_type++) {
#else
                            for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < MD_COMP_TYPES; cur_type++) {
#endif

                                // Skip the current compound type if not set to be injected
                                if (allowed_comp_types[cur_type] == 0)
                                    continue;
#if FTR_NEW_REF_PRUNING_CTRLS
                                if (!is_valid_bi_type(context_ptr, cur_type, list_idx_0, ref_idx_0, list_idx_1, ref_idx_1))
                                    continue;
#endif
                                cand_array[cand_idx].type = INTER_MODE;
#if !CLN_MD_CANDS
                                cand_array[cand_idx].inter_mode = NEAR_NEWMV;
#endif
                                cand_array[cand_idx].pred_mode = NEAR_NEWMV;
                                cand_array[cand_idx].motion_mode = SIMPLE_TRANSLATION;
                                cand_array[cand_idx].is_compound = 1;
                                cand_array[cand_idx].is_interintra_used = 0;
#if !CLN_MD_CANDS
                                cand_array[cand_idx].distortion_ready = 0;
#endif
                                cand_array[cand_idx].use_intrabc = 0;
                                cand_array[cand_idx].skip_mode_allowed = EB_FALSE;

                                cand_array[cand_idx].prediction_direction[0] = BI_PRED;
#if !CLN_MD_CANDS
                                cand_array[cand_idx].is_new_mv = 0;
#endif
                                cand_array[cand_idx].motion_vector_xl0 = to_inject_mv_x_l0;
                                cand_array[cand_idx].motion_vector_yl0 = to_inject_mv_y_l0;
                                cand_array[cand_idx].motion_vector_xl1 = to_inject_mv_x_l1;
                                cand_array[cand_idx].motion_vector_yl1 = to_inject_mv_y_l1;
                                cand_array[cand_idx].drl_index = drli;
#if !CLN_MD_CANDS
                                cand_array[cand_idx].ref_mv_index = 0;
#endif
                                cand_array[cand_idx].ref_frame_type = ref_pair;
#if !CLN_MD_CANDS
                                cand_array[cand_idx].ref_frame_index_l0 = ref_idx_0;
                                cand_array[cand_idx].ref_frame_index_l1 = ref_idx_1;
                                cand_array[cand_idx].transform_type[0] = DCT_DCT;
                                cand_array[cand_idx].transform_type_uv = DCT_DCT;
#endif
                                cand_array[cand_idx].motion_vector_pred_x[REF_LIST_1] =
                                    ref_mv[1].as_mv.col;
                                cand_array[cand_idx].motion_vector_pred_y[REF_LIST_1] =
                                    ref_mv[1].as_mv.row;

#if FTR_UPGRADE_COMP_LEVELS
                                if (cur_type > MD_COMP_AVG) {
                                    if (mask_done != 1) {
                                        if (calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_idx]))
                                            break;
                                        mask_done = 1;
                                    }
                                }
#else
                                context_ptr->injected_mv_x_bipred_l0_array
                                    [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l0;
                                context_ptr->injected_mv_y_bipred_l0_array
                                    [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l0;
                                context_ptr->injected_mv_x_bipred_l1_array
                                    [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l1;
                                context_ptr->injected_mv_y_bipred_l1_array
                                    [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l1;
                                context_ptr->injected_ref_type_bipred_array
                                    [context_ptr->injected_mv_count_bipred] = ref_pair;
                                ++context_ptr->injected_mv_count_bipred;

                                //NEAR_NEWMV
                                if (cur_type == MD_COMP_DIFF0 || cur_type == MD_COMP_WEDGE) {
                                    if (mask_done != 1) {
                                        calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_idx]);
                                        mask_done = 1;
                                    }
                                }
#endif
                                determine_compound_mode(
                                    pcs_ptr, context_ptr, &cand_array[cand_idx], cur_type);

                                INCRMENT_CAND_TOTAL_COUNT(cand_idx);
                            }
#if FTR_UPGRADE_COMP_LEVELS
                            context_ptr->injected_mv_x_bipred_l0_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l0;
                            context_ptr->injected_mv_y_bipred_l0_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l0;
                            context_ptr->injected_mv_x_bipred_l1_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l1;
                            context_ptr->injected_mv_y_bipred_l1_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l1;
                            context_ptr->injected_ref_type_bipred_array
                                [context_ptr->injected_mv_count_bipred] = ref_pair;
                            ++context_ptr->injected_mv_count_bipred;
#endif
                        }
                    }
                }
            }
        }
#if CLN_MD_CANDS
    }
#endif
    //update tot Candidate count
    *candTotCnt = cand_idx;
}
void inject_warped_motion_candidates(
    PictureControlSet              *pcs_ptr,
    struct ModeDecisionContext     *context_ptr,
    BlkStruct                      *blk_ptr,
    uint32_t                       *cand_tot_cnt,
    MeSbResults                    *me_results) {
    uint32_t can_idx = *cand_tot_cnt;
    ModeDecisionCandidate *cand_array = context_ptr->fast_candidate_array;
    MacroBlockD  *xd = blk_ptr->av1xd;
    uint8_t drli, max_drl_index;
    IntMv nearest_mv[2], near_mv[2], ref_mv[2];

    int inside_tile = 1;
    SequenceControlSet *scs_ptr =
        (SequenceControlSet *)pcs_ptr->parent_pcs_ptr->scs_wrapper_ptr->object_ptr;
    int umv0_tile = (scs_ptr->static_config.unrestricted_motion_vector == 0);
    uint32_t mi_row = context_ptr->blk_origin_y >> MI_SIZE_LOG2;
    uint32_t mi_col = context_ptr->blk_origin_x >> MI_SIZE_LOG2;
    uint32_t ref_it;
    MvReferenceFrame rf[2];
    Mv mv_0;
    MvUnit mv_unit;
    int16_t to_inject_mv_x, to_inject_mv_y;
    //all of ref pairs: (1)single-ref List0  (2)single-ref List1
    for (ref_it = 0; ref_it < pcs_ptr->parent_pcs_ptr->tot_ref_frame_types; ++ref_it) {
        MvReferenceFrame ref_frame_pair = pcs_ptr->parent_pcs_ptr->ref_frame_type_arr[ref_it];
        av1_set_ref_frame(rf, ref_frame_pair);

        //single ref/list
        if (rf[1] == NONE_FRAME)
        {
            MvReferenceFrame frame_type = rf[0];
            uint8_t list_idx = get_list_idx(rf[0]);
            uint8_t ref_idx = get_ref_frame_idx(rf[0]);
            if (!is_valid_unipred_ref(context_ptr, MIN(TOT_INTER_GROUP-1,WARP_GROUP), list_idx, ref_idx)) continue;
#if !FTR_NEW_REF_PRUNING_CTRLS
            if (ref_idx > context_ptr->md_max_ref_count - 1)
                continue;
#endif
#if OPT_WM
            // Get gm params
            if (context_ptr->cand_elimination_ctrs.inject_new_warp == 2) {
                EbWarpedMotionParams *gm_params = &pcs_ptr->parent_pcs_ptr->global_motion[frame_type];
                if (gm_params->wmtype <= TRANSLATION) continue;
            }
#endif
            //NEAREST
            to_inject_mv_x = context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                                 .ref_mvs[frame_type][0]
                                 .as_mv.col;
            to_inject_mv_y = context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                                 .ref_mvs[frame_type][0]
                                 .as_mv.row;

            if (umv0_tile)
                inside_tile = is_inside_tile_boundary(&(xd->tile), to_inject_mv_x, to_inject_mv_y, mi_col, mi_row, context_ptr->blk_geom->bsize);
            if (inside_tile)
            {
                cand_array[can_idx].type = INTER_MODE;
#if !CLN_MD_CANDS
                cand_array[can_idx].inter_mode = NEARESTMV;
#endif
                cand_array[can_idx].pred_mode = NEARESTMV;
                cand_array[can_idx].motion_mode = WARPED_CAUSAL;
                cand_array[can_idx].wm_params_l0.wmtype = AFFINE;
                cand_array[can_idx].is_compound = 0;
                cand_array[can_idx].is_interintra_used = 0;
#if !CLN_MD_CANDS
                cand_array[can_idx].distortion_ready = 0;
#endif
                cand_array[can_idx].use_intrabc = 0;
                cand_array[can_idx].skip_mode_allowed = EB_FALSE;
                cand_array[can_idx].prediction_direction[0] = list_idx;
#if !CLN_MD_CANDS
                cand_array[can_idx].is_new_mv = 0;
#endif
                if (list_idx == 0) {
                    cand_array[can_idx].motion_vector_xl0 = to_inject_mv_x;
                    cand_array[can_idx].motion_vector_yl0 = to_inject_mv_y;
                }
                else {
                    cand_array[can_idx].motion_vector_xl1 = to_inject_mv_x;
                    cand_array[can_idx].motion_vector_yl1 = to_inject_mv_y;
                }
                cand_array[can_idx].drl_index = 0;
#if !CLN_MD_CANDS
                cand_array[can_idx].ref_mv_index = 0;
#endif
                cand_array[can_idx].ref_frame_type = frame_type;
#if !CLN_MD_CANDS
                cand_array[can_idx].ref_frame_index_l0 = (list_idx == 0) ? ref_idx : -1;
                cand_array[can_idx].ref_frame_index_l1 = (list_idx == 1) ? ref_idx : -1;
                cand_array[can_idx].transform_type[0] = DCT_DCT;
                cand_array[can_idx].transform_type_uv = DCT_DCT;
#endif
                mv_0.x = to_inject_mv_x;
                mv_0.y = to_inject_mv_y;
                mv_unit.mv[list_idx] = mv_0;
                mv_unit.pred_direction = cand_array[can_idx].prediction_direction[0];
                cand_array[can_idx].local_warp_valid = warped_motion_parameters(
                    pcs_ptr,
                    context_ptr->blk_ptr,
                    &mv_unit,
                    context_ptr->blk_geom,
                    context_ptr->blk_origin_x,
                    context_ptr->blk_origin_y,
                    cand_array[can_idx].ref_frame_type,
                    &cand_array[can_idx].wm_params_l0,
                    &cand_array[can_idx].num_proj_ref);

                if (cand_array[can_idx].local_warp_valid)
                    INCRMENT_CAND_TOTAL_COUNT(can_idx);
            }
            //NEAR
            max_drl_index = get_max_drl_index(xd->ref_mv_count[frame_type], NEARMV);
            for (drli = 0; drli < max_drl_index; drli++) {
                get_av1_mv_pred_drl(
                    context_ptr,
                    blk_ptr,
                    frame_type,
                    0,
                    NEARMV,
                    drli,
                    nearest_mv,
                    near_mv,
                    ref_mv);

                to_inject_mv_x = near_mv[0].as_mv.col;
                to_inject_mv_y = near_mv[0].as_mv.row;

                if (umv0_tile)
                    inside_tile = is_inside_tile_boundary(&(xd->tile), to_inject_mv_x, to_inject_mv_y, mi_col, mi_row, context_ptr->blk_geom->bsize);
                if (inside_tile)
                {
                    cand_array[can_idx].type = INTER_MODE;
#if !CLN_MD_CANDS
                    cand_array[can_idx].inter_mode = NEARMV;
#endif
                    cand_array[can_idx].pred_mode = NEARMV;
                    cand_array[can_idx].motion_mode = WARPED_CAUSAL;
                    cand_array[can_idx].wm_params_l0.wmtype = AFFINE;
                    cand_array[can_idx].is_compound = 0;
                    cand_array[can_idx].is_interintra_used = 0;
#if !CLN_MD_CANDS
                    cand_array[can_idx].distortion_ready = 0;
#endif
                    cand_array[can_idx].use_intrabc = 0;
                    cand_array[can_idx].skip_mode_allowed = EB_FALSE;
                    cand_array[can_idx].prediction_direction[0] = list_idx;
#if !CLN_MD_CANDS
                    cand_array[can_idx].is_new_mv = 0;
#endif
                    if (list_idx == 0) {
                        cand_array[can_idx].motion_vector_xl0 = to_inject_mv_x;
                        cand_array[can_idx].motion_vector_yl0 = to_inject_mv_y;
                    }
                    else {
                        cand_array[can_idx].motion_vector_xl1 = to_inject_mv_x;
                        cand_array[can_idx].motion_vector_yl1 = to_inject_mv_y;
                    }
                    cand_array[can_idx].drl_index = drli;
#if !CLN_MD_CANDS
                    cand_array[can_idx].ref_mv_index = 0;
#endif
                    cand_array[can_idx].ref_frame_type = frame_type;
#if !CLN_MD_CANDS
                    cand_array[can_idx].ref_frame_index_l0 = (list_idx == 0) ? ref_idx : -1;
                    cand_array[can_idx].ref_frame_index_l1 = (list_idx == 1) ? ref_idx : -1;
                    cand_array[can_idx].transform_type[0] = DCT_DCT;
                    cand_array[can_idx].transform_type_uv = DCT_DCT;
#endif
                    mv_0.x = to_inject_mv_x;
                    mv_0.y = to_inject_mv_y;
                    mv_unit.mv[list_idx] = mv_0;
                    mv_unit.pred_direction = cand_array[can_idx].prediction_direction[0];

                    cand_array[can_idx].local_warp_valid = warped_motion_parameters(
                        pcs_ptr,
                        context_ptr->blk_ptr,
                        &mv_unit,
                        context_ptr->blk_geom,
                        context_ptr->blk_origin_x,
                        context_ptr->blk_origin_y,
                        cand_array[can_idx].ref_frame_type,
                        &cand_array[can_idx].wm_params_l0,
                        &cand_array[can_idx].num_proj_ref);

                    if (cand_array[can_idx].local_warp_valid)
                        INCRMENT_CAND_TOTAL_COUNT(can_idx);
                }
            }
        }
    }
    // NEWMV L0
#define NUM_WM_NEIGHBOUR_POS 13
    const MV neighbors[NUM_WM_NEIGHBOUR_POS] = {
        {0, 0}, {0, -1}, {1, 0}, {0, 1}, {-1, 0}, {0, -2}, {2, 0}, {0, 2}, {-2, 0}, {1, 1}, {-1, 1}, {1, -1}, {-1, 1} };
    IntMv  best_pred_mv[2] = { {0}, {0} };

    uint8_t total_me_cnt = me_results->total_me_candidate_index[context_ptr->me_block_offset];
    const MeCandidate *me_block_results = &me_results->me_candidate_array[context_ptr->me_cand_offset];

    for (uint8_t me_candidate_index = 0; me_candidate_index < total_me_cnt; ++me_candidate_index)
    {
        const MeCandidate *me_block_results_ptr = &me_block_results[me_candidate_index];
        const uint8_t inter_direction = me_block_results_ptr->direction;
        const uint8_t list0_ref_index = me_block_results_ptr->ref_idx_l0;
        const uint8_t list1_ref_index = me_block_results_ptr->ref_idx_l1;

        /**************
            NEWMV L0
        ************* */
        if (inter_direction == 0) {
            if (!is_valid_unipred_ref(context_ptr, MIN(TOT_INTER_GROUP-1,WARP_GROUP), REF_LIST_0, list0_ref_index)) continue;
#if !FTR_NEW_REF_PRUNING_CTRLS
            if (list0_ref_index > context_ptr->md_max_ref_count - 1)
                continue;
#endif
            to_inject_mv_x =
                context_ptr
                ->sb_me_mv[context_ptr->blk_geom->blkidx_mds][REF_LIST_0][list0_ref_index][0];
            to_inject_mv_y =
                context_ptr
                ->sb_me_mv[context_ptr->blk_geom->blkidx_mds][REF_LIST_0][list0_ref_index][1];
#if FTR_PD2_REDUCE_MDS0
            uint8_t num_wm_neighbour_pos = context_ptr->inject_new_warp == 1 ? 13 : context_ptr->inject_new_warp == 2 ? 9 : 1;
            for (int i = 0; i < num_wm_neighbour_pos; i++) {
#else
                for (int i = 0; i < NUM_WM_NEIGHBOUR_POS; i++) {
#endif
                    cand_array[can_idx].type = INTER_MODE;
#if !CLN_MD_CANDS
                    cand_array[can_idx].distortion_ready = 0;
#endif
                    cand_array[can_idx].use_intrabc = 0;
                    cand_array[can_idx].skip_mode_allowed = EB_FALSE;
                    cand_array[can_idx].prediction_direction[0] = (EbPredDirection)0;
#if !CLN_MD_CANDS
                    cand_array[can_idx].inter_mode = NEWMV;
#endif
                    cand_array[can_idx].pred_mode = NEWMV;
                    cand_array[can_idx].motion_mode = WARPED_CAUSAL;
                    cand_array[can_idx].wm_params_l0.wmtype = AFFINE;
                    cand_array[can_idx].is_compound = 0;
                    cand_array[can_idx].is_interintra_used = 0;
#if !CLN_MD_CANDS
                    cand_array[can_idx].is_new_mv = 1;
#endif
                    cand_array[can_idx].drl_index = 0;

                    if (pcs_ptr->parent_pcs_ptr->frm_hdr.allow_high_precision_mv) {
                        cand_array[can_idx].motion_vector_xl0 = to_inject_mv_x + neighbors[i].col;
                        cand_array[can_idx].motion_vector_yl0 = to_inject_mv_y + neighbors[i].row;
                    }
                    else {
                        cand_array[can_idx].motion_vector_xl0 = to_inject_mv_x + (neighbors[i].col << 1);
                        cand_array[can_idx].motion_vector_yl0 = to_inject_mv_y + (neighbors[i].row << 1);
                    }
#if !CLN_MD_CANDS
                    cand_array[can_idx].ref_mv_index = 0;
#endif
                    cand_array[can_idx].ref_frame_type = svt_get_ref_frame_type(REF_LIST_0, list0_ref_index);
#if !CLN_MD_CANDS
                    cand_array[can_idx].ref_frame_index_l0 = list0_ref_index;
                    cand_array[can_idx].ref_frame_index_l1 = -1;
                    cand_array[can_idx].transform_type[0] = DCT_DCT;
                    cand_array[can_idx].transform_type_uv = DCT_DCT;
#endif
                    choose_best_av1_mv_pred(
                        context_ptr,
#if CLN_FAST_COST
                        context_ptr->md_rate_estimation_ptr,
#else
                        cand_array[can_idx].md_rate_estimation_ptr,
#endif
                        context_ptr->blk_ptr,
                        cand_array[can_idx].ref_frame_type,
                        cand_array[can_idx].is_compound,
                        cand_array[can_idx].pred_mode,
                        cand_array[can_idx].motion_vector_xl0,
                        cand_array[can_idx].motion_vector_yl0,
                        0, 0,
                        &cand_array[can_idx].drl_index,
                        best_pred_mv);

                    cand_array[can_idx].motion_vector_pred_x[REF_LIST_0] = best_pred_mv[0].as_mv.col;
                    cand_array[can_idx].motion_vector_pred_y[REF_LIST_0] = best_pred_mv[0].as_mv.row;
                    mv_0.x = cand_array[can_idx].motion_vector_xl0;
                    mv_0.y = cand_array[can_idx].motion_vector_yl0;
                    mv_unit.mv[0] = mv_0;
                    mv_unit.pred_direction = cand_array[can_idx].prediction_direction[0];
                    if (umv0_tile)
                        inside_tile = is_inside_tile_boundary(&(xd->tile), mv_0.x, mv_0.y, mi_col, mi_row, context_ptr->blk_geom->bsize);
                    if (inside_tile)
                    {
                        cand_array[can_idx].local_warp_valid = warped_motion_parameters(
                            pcs_ptr,
                            context_ptr->blk_ptr,
                            &mv_unit,
                            context_ptr->blk_geom,
                            context_ptr->blk_origin_x,
                            context_ptr->blk_origin_y,
                            cand_array[can_idx].ref_frame_type,
                            &cand_array[can_idx].wm_params_l0,
                            &cand_array[can_idx].num_proj_ref);

                        if (cand_array[can_idx].local_warp_valid)
                            INCRMENT_CAND_TOTAL_COUNT(can_idx);
                    }
                }
        }
        /**************
           NEWMV L1
       ************* */
        if (inter_direction == 1) {
            if (!is_valid_unipred_ref(context_ptr, MIN(TOT_INTER_GROUP-1,WARP_GROUP), REF_LIST_1, list1_ref_index)) continue;
#if !FTR_NEW_REF_PRUNING_CTRLS
            if (list1_ref_index > context_ptr->md_max_ref_count - 1)
                continue;
#endif
            to_inject_mv_x =
                context_ptr
                ->sb_me_mv[context_ptr->blk_geom->blkidx_mds][REF_LIST_1][list1_ref_index][0];
            to_inject_mv_y =
                context_ptr
                ->sb_me_mv[context_ptr->blk_geom->blkidx_mds][REF_LIST_1][list1_ref_index][1];
#if FTR_PD2_REDUCE_MDS0
            uint8_t num_wm_neighbour_pos = context_ptr->inject_new_warp == 1 ? 13 : context_ptr->inject_new_warp == 2 ? 9 : 1;
                for (int i = 0; i < num_wm_neighbour_pos; i++) {
#else
                for (int i = 0; i < NUM_WM_NEIGHBOUR_POS; i++) {
#endif

                    cand_array[can_idx].type = INTER_MODE;
#if !CLN_MD_CANDS
                    cand_array[can_idx].distortion_ready = 0;
#endif
                    cand_array[can_idx].use_intrabc = 0;
                    cand_array[can_idx].skip_mode_allowed = EB_FALSE;
                    cand_array[can_idx].prediction_direction[0] = (EbPredDirection)1;
#if !CLN_MD_CANDS
                    cand_array[can_idx].inter_mode = NEWMV;
#endif
                    cand_array[can_idx].pred_mode = NEWMV;
                    cand_array[can_idx].motion_mode = WARPED_CAUSAL;
                    cand_array[can_idx].wm_params_l0.wmtype = AFFINE;

                    cand_array[can_idx].is_compound = 0;
                    cand_array[can_idx].is_interintra_used = 0;
#if !CLN_MD_CANDS
                    cand_array[can_idx].is_new_mv = 1;
#endif
                    cand_array[can_idx].drl_index = 0;

                    if (pcs_ptr->parent_pcs_ptr->frm_hdr.allow_high_precision_mv) {
                        cand_array[can_idx].motion_vector_xl1 = to_inject_mv_x + neighbors[i].col;
                        cand_array[can_idx].motion_vector_yl1 = to_inject_mv_y + neighbors[i].row;
                    }
                    else {
                        cand_array[can_idx].motion_vector_xl1 = to_inject_mv_x + (neighbors[i].col << 1);
                        cand_array[can_idx].motion_vector_yl1 = to_inject_mv_y + (neighbors[i].row << 1);
                    }
#if !CLN_MD_CANDS
                    cand_array[can_idx].ref_mv_index = 0;
#endif
                    cand_array[can_idx].ref_frame_type = svt_get_ref_frame_type(REF_LIST_1, list1_ref_index);
#if !CLN_MD_CANDS
                    cand_array[can_idx].ref_frame_index_l0 = -1;
                    cand_array[can_idx].ref_frame_index_l1 = list1_ref_index;
                    cand_array[can_idx].transform_type[0] = DCT_DCT;
                    cand_array[can_idx].transform_type_uv = DCT_DCT;
#endif
                    choose_best_av1_mv_pred(
                        context_ptr,
#if CLN_FAST_COST
                        context_ptr->md_rate_estimation_ptr,
#else
                        cand_array[can_idx].md_rate_estimation_ptr,
#endif
                        context_ptr->blk_ptr,
                        cand_array[can_idx].ref_frame_type,
                        cand_array[can_idx].is_compound,
                        cand_array[can_idx].pred_mode,
                        cand_array[can_idx].motion_vector_xl1,
                        cand_array[can_idx].motion_vector_yl1,
                        0,
                        0,
                        &cand_array[can_idx].drl_index,
                        best_pred_mv);

                    cand_array[can_idx].motion_vector_pred_x[REF_LIST_1] = best_pred_mv[0].as_mv.col;
                    cand_array[can_idx].motion_vector_pred_y[REF_LIST_1] = best_pred_mv[0].as_mv.row;

                    mv_0.x = cand_array[can_idx].motion_vector_xl1;
                    mv_0.y = cand_array[can_idx].motion_vector_yl1;
                    mv_unit.mv[1] = mv_0;
                    mv_unit.pred_direction = cand_array[can_idx].prediction_direction[0];
                    if (umv0_tile)
                        inside_tile = is_inside_tile_boundary(&(xd->tile), mv_0.x, mv_0.y, mi_col, mi_row, context_ptr->blk_geom->bsize);
                    if (inside_tile)
                    {
                        cand_array[can_idx].local_warp_valid = warped_motion_parameters(
                            pcs_ptr,
                            context_ptr->blk_ptr,
                            &mv_unit,
                            context_ptr->blk_geom,
                            context_ptr->blk_origin_x,
                            context_ptr->blk_origin_y,
                            cand_array[can_idx].ref_frame_type,
                            &cand_array[can_idx].wm_params_l0,
                            &cand_array[can_idx].num_proj_ref);

                        if (cand_array[can_idx].local_warp_valid)
                            INCRMENT_CAND_TOTAL_COUNT(can_idx);
                    }
                }
        }
    }

    *cand_tot_cnt = can_idx;
}
static INLINE void setup_pred_plane(struct Buf2D *dst, BlockSize bsize, uint8_t *src, int width,
                                    int height, int stride, int mi_row, int mi_col,
                                    int subsampling_x, int subsampling_y) {
    // Offset the buffer pointer
    if (subsampling_y && (mi_row & 0x01) && (mi_size_high[bsize] == 1)) mi_row -= 1;
    if (subsampling_x && (mi_col & 0x01) && (mi_size_wide[bsize] == 1)) mi_col -= 1;

    const int x = (MI_SIZE * mi_col) >> subsampling_x;
    const int y = (MI_SIZE * mi_row) >> subsampling_y;
    dst->buf    = src + (y * stride + x); // scaled_buffer_offset(x, y, stride, scale);
    dst->buf0   = src;
    dst->width  = width;
    dst->height = height;
    dst->stride = stride;
}
void svt_av1_setup_pred_block(BlockSize sb_type, struct Buf2D dst[MAX_MB_PLANE],
                              const Yv12BufferConfig *src, int mi_row, int mi_col) {

    dst[0].buf    = src->y_buffer;
    dst[0].stride = src->y_stride;
    dst[1].buf    = src->u_buffer;
    dst[2].buf    = src->v_buffer;
    dst[1].stride = dst[2].stride = src->uv_stride;

    setup_pred_plane(dst,
                     sb_type,
                     dst[0].buf,
                     src->y_crop_width,
                     src->y_crop_height,
                     dst[0].stride,
                     mi_row,
                     mi_col,
                     0,
                     0);
}

// Values are now correlated to quantizer.
static int sad_per_bit16lut_8[QINDEX_RANGE];
static int sad_per_bit_lut_10[QINDEX_RANGE];
extern AomVarianceFnPtr mefn_ptr[BlockSizeS_ALL];

int svt_av1_find_best_obmc_sub_pixel_tree_up(ModeDecisionContext *context_ptr, IntraBcContext *x,
                                             const AV1_COMMON *const cm, int mi_row, int mi_col,
                                             MV *bestmv, const MV *ref_mv, int allow_hp,
                                             int error_per_bit, const AomVarianceFnPtr *vfp,
                                             int forced_stop, int iters_per_step, int *mvjcost,
                                             int *mvcost[2], int *distortion, unsigned int *sse1,
                                             int is_second, int use_accurate_subpel_search);

int svt_av1_obmc_full_pixel_search(ModeDecisionContext *context_ptr, IntraBcContext *x, MV *mvp_full,
                                   int sadpb, const AomVarianceFnPtr *fn_ptr, const MV *ref_mv,
                                   MV *dst_mv, int is_second);

static void single_motion_search(PictureControlSet *pcs, ModeDecisionContext *context_ptr,
                                 ModeDecisionCandidate *candidate_ptr, const MvReferenceFrame *rf,
                                 IntMv best_pred_mv, IntraBcContext *x, BlockSize bsize, MV *ref_mv,
                                 int ref_idx, int *rate_mv) {
    (void)ref_idx;
    const Av1Common *const cm      = pcs->parent_pcs_ptr->av1_cm;
    FrameHeader *          frm_hdr = &pcs->parent_pcs_ptr->frm_hdr;
// single_motion_search supports 8bit path only
    uint32_t full_lambda = context_ptr->full_lambda_md[EB_8_BIT_MD];

    x->xd            = context_ptr->blk_ptr->av1xd;
    const int mi_row = -x->xd->mb_to_top_edge / (8 * MI_SIZE);
    const int mi_col = -x->xd->mb_to_left_edge / (8 * MI_SIZE);

    x->nmv_vec_cost  = context_ptr->md_rate_estimation_ptr->nmv_vec_cost;
    x->mv_cost_stack = context_ptr->md_rate_estimation_ptr->nmvcoststack;
    // Set up limit values for MV components.
    // Mv beyond the range do not produce new/different prediction block.
    const int mi_width   = mi_size_wide[bsize];
    const int mi_height  = mi_size_high[bsize];
    x->mv_limits.row_min = -(((mi_row + mi_height) * MI_SIZE) + AOM_INTERP_EXTEND);
    x->mv_limits.col_min = -(((mi_col + mi_width) * MI_SIZE) + AOM_INTERP_EXTEND);
    x->mv_limits.row_max = (cm->mi_rows - mi_row) * MI_SIZE + AOM_INTERP_EXTEND;
    x->mv_limits.col_max = (cm->mi_cols - mi_col) * MI_SIZE + AOM_INTERP_EXTEND;
    //set search paramters
    x->sadperbit16 = sad_per_bit16lut_8[frm_hdr->quantization_params.base_q_idx];
    x->errorperbit = full_lambda >> RD_EPB_SHIFT;
    x->errorperbit += (x->errorperbit == 0);

    int bestsme = INT_MAX;
    int sadpb   = x->sadperbit16;
    MV  mvp_full;

    MvLimits tmp_mv_limits = x->mv_limits;

    // Note: MV limits are modified here. Always restore the original values
    // after full-pixel motion search.
    svt_av1_set_mv_search_range(&x->mv_limits, ref_mv);

    mvp_full = best_pred_mv.as_mv; // mbmi->mv[0].as_mv;

    mvp_full.col >>= 3;
    mvp_full.row >>= 3;

    x->best_mv.as_int = x->second_best_mv.as_int = INVALID_MV; //D

    switch (candidate_ptr->motion_mode) {
    case OBMC_CAUSAL:
        bestsme = svt_av1_obmc_full_pixel_search(
            context_ptr, x, &mvp_full, sadpb, &mefn_ptr[bsize], ref_mv, &(x->best_mv.as_mv), 0);
        break;
    default: assert(0 && "Invalid motion mode!\n");
    }

    x->mv_limits = tmp_mv_limits;

    const int use_fractional_mv = bestsme < INT_MAX && frm_hdr->force_integer_mv == 0;
    if (use_fractional_mv) {
        int dis; /* TODO: use dis in distortion calculation later. */
        switch (candidate_ptr->motion_mode) {
        case OBMC_CAUSAL:
            svt_av1_find_best_obmc_sub_pixel_tree_up(context_ptr,
                                                     x,
                                                     cm,
                                                     mi_row,
                                                     mi_col,
                                                     &x->best_mv.as_mv,
                                                     ref_mv,
                                                     frm_hdr->allow_high_precision_mv,
                                                     x->errorperbit,
                                                     &mefn_ptr[bsize],
                                                     0, // mv.subpel_force_stop
                                                     2, //  mv.subpel_iters_per_step
                                                     x->nmv_vec_cost,
                                                     x->mv_cost_stack,
                                                     &dis,
                                                     &context_ptr->pred_sse[rf[0]],
                                                     0,
                                                     USE_8_TAPS);
            break;
        default: assert(0 && "Invalid motion mode!\n");
        }
    }
    *rate_mv = svt_av1_mv_bit_cost(
        &x->best_mv.as_mv, ref_mv, x->nmv_vec_cost, x->mv_cost_stack, MV_COST_WEIGHT);
}

void obmc_motion_refinement(PictureControlSet *pcs_ptr, struct ModeDecisionContext *context_ptr,
                            ModeDecisionCandidate *candidate, uint8_t ref_list_idx) {
    IntMv           best_pred_mv[2] = {{0}, {0}};
    IntraBcContext  x_st;
    IntraBcContext *x = &x_st;

    MacroBlockD *xd;
    xd = x->xd       = context_ptr->blk_ptr->av1xd;
    const int mi_row = -xd->mb_to_top_edge / (8 * MI_SIZE);
    const int mi_col = -xd->mb_to_left_edge / (8 * MI_SIZE);

    {
        uint8_t              ref_idx  = get_ref_frame_idx(candidate->ref_frame_type);
        uint8_t              list_idx = get_list_idx(candidate->ref_frame_type);

        assert(list_idx < MAX_NUM_OF_REF_PIC_LIST);
        EbPictureBufferDesc *reference_picture =
            ((EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[list_idx][ref_idx]->object_ptr)
                ->reference_picture;

        use_scaled_rec_refs_if_needed(pcs_ptr,
                                      pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr,
                                      (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[list_idx][ref_idx]->object_ptr,
                                      &reference_picture);

        Yv12BufferConfig ref_buf;
        link_eb_to_aom_buffer_desc_8bit(reference_picture, &ref_buf);

        struct Buf2D yv12_mb[MAX_MB_PLANE];
        svt_av1_setup_pred_block(context_ptr->blk_geom->bsize, yv12_mb, &ref_buf, mi_row, mi_col);
        for (int i = 0; i < 1; ++i) x->xdplane[i].pre[0] = yv12_mb[i]; //ref in ME

        x->plane[0].src.buf  = 0; // x->xdplane[0].pre[0];
        x->plane[0].src.buf0 = 0;
    }

    IntMv best_mv;
    best_mv.as_int = 0;
    if (ref_list_idx == 0) {
        best_mv.as_mv.col = candidate->motion_vector_xl0; // to_inject_mv_x;
        best_mv.as_mv.row = candidate->motion_vector_yl0; //to_inject_mv_y;
    } else {
        best_mv.as_mv.col = candidate->motion_vector_xl1; // to_inject_mv_x;
        best_mv.as_mv.row = candidate->motion_vector_yl1; //to_inject_mv_y;
    }
    int tmp_rate_mv;

    MV ref_mv;
    ref_mv.col = candidate->motion_vector_pred_x[ref_list_idx];
    ref_mv.row = candidate->motion_vector_pred_y[ref_list_idx];

    single_motion_search(pcs_ptr,
                         context_ptr,
                         candidate,
                         (const MvReferenceFrame[]){candidate->ref_frame_type, -1},
                         best_mv,
                         x,
                         context_ptr->blk_geom->bsize,
                         &ref_mv,
                         0,
                         &tmp_rate_mv);

    if (ref_list_idx == 0) {
        candidate->motion_vector_xl0 = x->best_mv.as_mv.col;
        candidate->motion_vector_yl0 = x->best_mv.as_mv.row;
    } else {
        candidate->motion_vector_xl1 = x->best_mv.as_mv.col;
        candidate->motion_vector_yl1 = x->best_mv.as_mv.row;
    }

    choose_best_av1_mv_pred(
        context_ptr,
#if CLN_FAST_COST
        context_ptr->md_rate_estimation_ptr,
#else
        candidate->md_rate_estimation_ptr,
#endif
        context_ptr->blk_ptr,
        candidate->ref_frame_type,
        candidate->is_compound,
        candidate->pred_mode,
        ref_list_idx == 0 ? candidate->motion_vector_xl0 : candidate->motion_vector_xl1,
        ref_list_idx == 0 ? candidate->motion_vector_yl0 : candidate->motion_vector_yl1,
        0,
        0,
        &candidate->drl_index,
        best_pred_mv);

    if (ref_list_idx == 0) {
        candidate->motion_vector_pred_x[REF_LIST_0] = best_pred_mv[0].as_mv.col;
        candidate->motion_vector_pred_y[REF_LIST_0] = best_pred_mv[0].as_mv.row;
    } else {
        candidate->motion_vector_pred_x[REF_LIST_1] = best_pred_mv[0].as_mv.col;
        candidate->motion_vector_pred_y[REF_LIST_1] = best_pred_mv[0].as_mv.row;
    }
}

void inject_new_candidates(const SequenceControlSet *  scs_ptr,
                           struct ModeDecisionContext *context_ptr, PictureControlSet *pcs_ptr,
                           EbBool is_compound_enabled, EbBool allow_bipred, uint32_t me_sb_addr,
                           uint32_t me_block_offset, uint32_t *candidate_total_cnt) {
    ModeDecisionCandidate *cand_array      = context_ptr->fast_candidate_array;
    IntMv                  best_pred_mv[2] = {{0}, {0}};
    uint32_t               cand_total_cnt  = (*candidate_total_cnt);
    const MeSbResults *me_results =
        pcs_ptr->parent_pcs_ptr->pa_me_data->me_results[me_sb_addr];
    uint8_t            total_me_cnt     = me_results->total_me_candidate_index[me_block_offset];
    const MeCandidate *me_block_results = &me_results->me_candidate_array[context_ptr->me_cand_offset];
    MacroBlockD *      xd               = context_ptr->blk_ptr->av1xd;
    int                inside_tile      = 1;
    int                umv0tile         = (scs_ptr->static_config.unrestricted_motion_vector == 0);
    uint32_t           mi_row           = context_ptr->blk_origin_y >> MI_SIZE_LOG2;
    uint32_t           mi_col           = context_ptr->blk_origin_x >> MI_SIZE_LOG2;
    BlockSize          bsize            = context_ptr->blk_geom->bsize; // bloc size
#if FTR_UPGRADE_COMP_LEVELS
    MD_COMP_TYPE tot_comp_types =
        (context_ptr->inter_comp_ctrls.do_me == 0)
        ? MD_COMP_DIST
        : MD_COMP_TYPES;
#endif
#if OPT_ME
    const uint8_t max_refs = pcs_ptr->parent_pcs_ptr->pa_me_data->max_refs;
    const uint8_t max_l0 = pcs_ptr->parent_pcs_ptr->pa_me_data->max_l0;
#endif
    for (uint8_t me_candidate_index = 0; me_candidate_index < total_me_cnt; ++me_candidate_index) {
        const MeCandidate *me_block_results_ptr = &me_block_results[me_candidate_index];
        const uint8_t      inter_direction      = me_block_results_ptr->direction;
        const uint8_t      list0_ref_index      = me_block_results_ptr->ref_idx_l0;
        const uint8_t      list1_ref_index      = me_block_results_ptr->ref_idx_l1;

        /**************
            NEWMV L0
        ************* */
        if (inter_direction == 0) {
            if (!is_valid_unipred_ref(context_ptr, MIN(TOT_INTER_GROUP-1,PA_ME_GROUP), REF_LIST_0, list0_ref_index))
                continue;
#if !FTR_NEW_REF_PRUNING_CTRLS
            if (list0_ref_index > context_ptr->md_max_ref_count - 1) continue;
#endif
            int16_t to_inject_mv_x =
                context_ptr
                ->sb_me_mv[context_ptr->blk_geom->blkidx_mds][REF_LIST_0][list0_ref_index][0];
            int16_t to_inject_mv_y =
                context_ptr
                ->sb_me_mv[context_ptr->blk_geom->blkidx_mds][REF_LIST_0][list0_ref_index][1];
            uint8_t to_inject_ref_type = svt_get_ref_frame_type(REF_LIST_0, list0_ref_index);
            inside_tile = 1;
            if (umv0tile)
                inside_tile = is_inside_tile_boundary(&(xd->tile),
                                                      to_inject_mv_x,
                                                      to_inject_mv_y,
                                                      mi_col,
                                                      mi_row,
                                                      context_ptr->blk_geom->bsize);
            uint8_t skip_cand = (!inside_tile);

            if (!skip_cand &&
                (context_ptr->injected_mv_count_l0 == 0 ||
                 mrp_is_already_injected_mv_l0(
                     context_ptr, to_inject_mv_x, to_inject_mv_y, to_inject_ref_type) ==
                     EB_FALSE)) {
                uint8_t inter_type;
                uint8_t is_ii_allowed =
                    svt_is_interintra_allowed(context_ptr->inter_intra_comp_ctrls.enabled, bsize, NEWMV, (const MvReferenceFrame[]) { to_inject_ref_type, -1 });
#if !FTR_NEW_REF_PRUNING_CTRLS // intra-inter
                if (is_ii_allowed && context_ptr->inter_intra_comp_ctrls.closest_ref_only)
                    if (list0_ref_index > 0)
                        is_ii_allowed = 0;
#endif
                uint8_t tot_inter_types = is_ii_allowed ? II_COUNT : 1;
                uint8_t is_obmc_allowed =
                    obmc_motion_mode_allowed(
                        pcs_ptr, context_ptr, bsize, to_inject_ref_type, -1, NEWMV) == OBMC_CAUSAL;
#if !FTR_NEW_REF_PRUNING_CTRLS // obmc
                is_obmc_allowed = context_ptr->obmc_ctrls.me_count == 0 ? 0 :
                    me_candidate_index > context_ptr->obmc_ctrls.me_count - 1 ? 0 :
                    is_obmc_allowed;
#endif
                tot_inter_types = is_obmc_allowed ? tot_inter_types + 1 : tot_inter_types;
#if FTR_UPGRADE_COMP_LEVELS
                uint8_t drl_index = 0;
                choose_best_av1_mv_pred(context_ptr,
                    context_ptr->md_rate_estimation_ptr,
                    context_ptr->blk_ptr,
                    to_inject_ref_type,
                    0,
                    NEWMV,
                    to_inject_mv_x,
                    to_inject_mv_y,
                    0,
                    0,
                    &drl_index,
                    best_pred_mv);
#endif
                for (inter_type = 0; inter_type < tot_inter_types; inter_type++) {
#if FTR_NEW_REF_PRUNING_CTRLS
                    if (!is_valid_uni_type(context_ptr, inter_type, is_ii_allowed, 0, REF_LIST_0, list0_ref_index))
                        continue;
#endif
                    cand_array[cand_total_cnt].type                    = INTER_MODE;
#if !CLN_MD_CANDS
                    cand_array[cand_total_cnt].distortion_ready        = 0;
#endif
                    cand_array[cand_total_cnt].use_intrabc             = 0;
                    cand_array[cand_total_cnt].skip_mode_allowed              = EB_FALSE;
                    cand_array[cand_total_cnt].prediction_direction[0] = (EbPredDirection)0;
#if !CLN_MD_CANDS
                    cand_array[cand_total_cnt].inter_mode              = NEWMV;
#endif
                    cand_array[cand_total_cnt].pred_mode               = NEWMV;
                    cand_array[cand_total_cnt].motion_mode             = SIMPLE_TRANSLATION;
                    cand_array[cand_total_cnt].is_compound             = 0;
#if !CLN_MD_CANDS
                    cand_array[cand_total_cnt].is_new_mv               = 1;
#endif
#if FTR_UPGRADE_COMP_LEVELS
                    cand_array[cand_total_cnt].drl_index = drl_index;
#else
                    cand_array[cand_total_cnt].drl_index               = 0;
#endif

                    // Set the MV to ME result
                    cand_array[cand_total_cnt].motion_vector_xl0 = to_inject_mv_x;
                    cand_array[cand_total_cnt].motion_vector_yl0 = to_inject_mv_y;

                    // will be needed later by the rate estimation
#if CLN_MD_CANDS
                    cand_array[cand_total_cnt].ref_frame_type = to_inject_ref_type;
#else
                    cand_array[cand_total_cnt].ref_mv_index   = 0;
                    cand_array[cand_total_cnt].ref_frame_type =
                        svt_get_ref_frame_type(REF_LIST_0, list0_ref_index);
                    cand_array[cand_total_cnt].ref_frame_index_l0 = list0_ref_index;
                    cand_array[cand_total_cnt].ref_frame_index_l1 = -1;
                    cand_array[cand_total_cnt].transform_type[0] = DCT_DCT;
                    cand_array[cand_total_cnt].transform_type_uv = DCT_DCT;
#endif
#if !FTR_UPGRADE_COMP_LEVELS
                    choose_best_av1_mv_pred(context_ptr,
#if CLN_FAST_COST
                                            context_ptr->md_rate_estimation_ptr,
#else
                                            cand_array[cand_total_cnt].md_rate_estimation_ptr,
#endif
                                            context_ptr->blk_ptr,
                                            cand_array[cand_total_cnt].ref_frame_type,
                                            cand_array[cand_total_cnt].is_compound,
                                            cand_array[cand_total_cnt].pred_mode,
                                            cand_array[cand_total_cnt].motion_vector_xl0,
                                            cand_array[cand_total_cnt].motion_vector_yl0,
                                            0,
                                            0,
                                            &cand_array[cand_total_cnt].drl_index,
                                            best_pred_mv);
#endif
                    cand_array[cand_total_cnt].motion_vector_pred_x[REF_LIST_0] =
                        best_pred_mv[0].as_mv.col;
                    cand_array[cand_total_cnt].motion_vector_pred_y[REF_LIST_0] =
                        best_pred_mv[0].as_mv.row;

                    if (inter_type == 0) {
                        cand_array[cand_total_cnt].is_interintra_used = 0;
                        cand_array[cand_total_cnt].motion_mode        = SIMPLE_TRANSLATION;
                    } else {
                        if (is_ii_allowed) {
                            if (inter_type == 1) {
                                inter_intra_search(
                                    pcs_ptr, context_ptr, &cand_array[cand_total_cnt]);
                                cand_array[cand_total_cnt].is_interintra_used   = 1;
                                cand_array[cand_total_cnt].use_wedge_interintra = 1;
                            } else if (inter_type == 2) {
                                cand_array[cand_total_cnt].is_interintra_used = 1;
                                cand_array[cand_total_cnt].interintra_mode =
                                    cand_array[cand_total_cnt - 1].interintra_mode;
                                cand_array[cand_total_cnt].use_wedge_interintra = 0;
                            }
                        }

                        if (is_obmc_allowed && inter_type == tot_inter_types - 1) {
                            cand_array[cand_total_cnt].is_interintra_used = 0;
                            cand_array[cand_total_cnt].motion_mode        = OBMC_CAUSAL;

                            obmc_motion_refinement(
                                pcs_ptr, context_ptr, &cand_array[cand_total_cnt], REF_LIST_0);
                        }
                    }

                    INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
                }
                context_ptr->injected_mv_x_l0_array[context_ptr->injected_mv_count_l0] =
                    to_inject_mv_x;
                context_ptr->injected_mv_y_l0_array[context_ptr->injected_mv_count_l0] =
                    to_inject_mv_y;
                context_ptr->injected_ref_type_l0_array[context_ptr->injected_mv_count_l0] =
                    to_inject_ref_type;
                ++context_ptr->injected_mv_count_l0;
            }
        }

        if (is_compound_enabled) {
            /**************
               NEWMV L1
           ************* */
            if (inter_direction == 1) {
                if (!is_valid_unipred_ref(context_ptr, MIN(TOT_INTER_GROUP-1,PA_ME_GROUP), REF_LIST_1, list1_ref_index))
                    continue;
#if !FTR_NEW_REF_PRUNING_CTRLS
                if (list1_ref_index > context_ptr->md_max_ref_count - 1) continue;
#endif
                int16_t to_inject_mv_x = context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                    [REF_LIST_1][list1_ref_index][0];
                int16_t to_inject_mv_y = context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                    [REF_LIST_1][list1_ref_index][1];
                uint8_t to_inject_ref_type = svt_get_ref_frame_type(REF_LIST_1, list1_ref_index);

                inside_tile = 1;
                if (umv0tile)
                    inside_tile = is_inside_tile_boundary(&(xd->tile),
                                                          to_inject_mv_x,
                                                          to_inject_mv_y,
                                                          mi_col,
                                                          mi_row,
                                                          context_ptr->blk_geom->bsize);
                uint8_t skip_cand = !inside_tile;

                if (!skip_cand &&
                    (context_ptr->injected_mv_count_l1 == 0 ||
                     mrp_is_already_injected_mv_l1(
                         context_ptr, to_inject_mv_x, to_inject_mv_y, to_inject_ref_type) ==
                         EB_FALSE)) {

                    uint8_t inter_type;
                    uint8_t is_ii_allowed =
                        svt_is_interintra_allowed(context_ptr->inter_intra_comp_ctrls.enabled, bsize, NEWMV, (const MvReferenceFrame[]) { to_inject_ref_type, -1 });
#if !FTR_NEW_REF_PRUNING_CTRLS // intra-inter
                    if (is_ii_allowed && context_ptr->inter_intra_comp_ctrls.closest_ref_only)
                        if (list1_ref_index > 0)
                            is_ii_allowed = 0;
#endif
                    uint8_t tot_inter_types = is_ii_allowed ? II_COUNT : 1;
                    uint8_t is_obmc_allowed =
                        obmc_motion_mode_allowed(
                            pcs_ptr, context_ptr, bsize, to_inject_ref_type, -1, NEWMV) == OBMC_CAUSAL;
#if !FTR_NEW_REF_PRUNING_CTRLS // obmc
                    is_obmc_allowed =context_ptr->obmc_ctrls.me_count == 0 ? 0 :
                        me_candidate_index > context_ptr->obmc_ctrls.me_count - 1 ? 0 :
                        is_obmc_allowed;
#endif
                    tot_inter_types = is_obmc_allowed ? tot_inter_types + 1 : tot_inter_types;
#if FTR_UPGRADE_COMP_LEVELS
                    uint8_t drl_index = 0;
                    choose_best_av1_mv_pred(context_ptr,
                        context_ptr->md_rate_estimation_ptr,
                        context_ptr->blk_ptr,
                        to_inject_ref_type,
                        0,
                        NEWMV,
                        to_inject_mv_x,
                        to_inject_mv_y,
                        0,
                        0,
                        &drl_index,
                        best_pred_mv);
#endif
                    for (inter_type = 0; inter_type < tot_inter_types; inter_type++) {
#if FTR_NEW_REF_PRUNING_CTRLS
                        if (!is_valid_uni_type(context_ptr, inter_type, is_ii_allowed, 0, REF_LIST_1, list1_ref_index))
                            continue;
#endif
                        cand_array[cand_total_cnt].type                    = INTER_MODE;
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].distortion_ready        = 0;
#endif
                        cand_array[cand_total_cnt].use_intrabc             = 0;
                        cand_array[cand_total_cnt].skip_mode_allowed              = EB_FALSE;
                        cand_array[cand_total_cnt].prediction_direction[0] = (EbPredDirection)1;
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].inter_mode              = NEWMV;
#endif
                        cand_array[cand_total_cnt].pred_mode               = NEWMV;
                        cand_array[cand_total_cnt].motion_mode             = SIMPLE_TRANSLATION;
                        cand_array[cand_total_cnt].is_compound             = 0;
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].is_new_mv               = 1;
#endif
#if FTR_UPGRADE_COMP_LEVELS
                        cand_array[cand_total_cnt].drl_index               = drl_index;
#else
                        cand_array[cand_total_cnt].drl_index               = 0;
#endif

                        // Set the MV to ME result
                        cand_array[cand_total_cnt].motion_vector_xl1 = to_inject_mv_x;
                        cand_array[cand_total_cnt].motion_vector_yl1 = to_inject_mv_y;

                        // will be needed later by the rate estimation
#if CLN_MD_CANDS
                        cand_array[cand_total_cnt].ref_frame_type = to_inject_ref_type;
#else
                        cand_array[cand_total_cnt].ref_mv_index   = 0;
                        cand_array[cand_total_cnt].ref_frame_type =
                            svt_get_ref_frame_type(REF_LIST_1, list1_ref_index);
                        cand_array[cand_total_cnt].ref_frame_index_l0 = -1;
                        cand_array[cand_total_cnt].ref_frame_index_l1 = list1_ref_index;
                        cand_array[cand_total_cnt].transform_type[0] = DCT_DCT;
                        cand_array[cand_total_cnt].transform_type_uv = DCT_DCT;
#endif
#if !FTR_UPGRADE_COMP_LEVELS
                        choose_best_av1_mv_pred(context_ptr,
#if CLN_FAST_COST
                                                context_ptr->md_rate_estimation_ptr,
#else
                                                cand_array[cand_total_cnt].md_rate_estimation_ptr,
#endif
                                                context_ptr->blk_ptr,
                                                cand_array[cand_total_cnt].ref_frame_type,
                                                cand_array[cand_total_cnt].is_compound,
                                                cand_array[cand_total_cnt].pred_mode,
                                                cand_array[cand_total_cnt].motion_vector_xl1,
                                                cand_array[cand_total_cnt].motion_vector_yl1,
                                                0,
                                                0,
                                                &cand_array[cand_total_cnt].drl_index,
                                                best_pred_mv);
#endif
                        cand_array[cand_total_cnt].motion_vector_pred_x[REF_LIST_1] =
                            best_pred_mv[0].as_mv.col;
                        cand_array[cand_total_cnt].motion_vector_pred_y[REF_LIST_1] =
                            best_pred_mv[0].as_mv.row;
                        if (inter_type == 0) {
                            cand_array[cand_total_cnt].is_interintra_used = 0;
                            cand_array[cand_total_cnt].motion_mode        = SIMPLE_TRANSLATION;
                        } else {
                            if (is_ii_allowed) {
                                if (inter_type == 1) {
                                    inter_intra_search(
                                        pcs_ptr, context_ptr, &cand_array[cand_total_cnt]);
                                    cand_array[cand_total_cnt].is_interintra_used   = 1;
                                    cand_array[cand_total_cnt].use_wedge_interintra = 1;
                                } else if (inter_type == 2) {
                                    cand_array[cand_total_cnt].is_interintra_used = 1;
                                    cand_array[cand_total_cnt].interintra_mode =
                                        cand_array[cand_total_cnt - 1].interintra_mode;
                                    cand_array[cand_total_cnt].use_wedge_interintra = 0;
                                }
                            }
                            if (is_obmc_allowed && inter_type == tot_inter_types - 1) {
                                cand_array[cand_total_cnt].is_interintra_used = 0;
                                cand_array[cand_total_cnt].motion_mode        = OBMC_CAUSAL;

                                obmc_motion_refinement(
                                    pcs_ptr, context_ptr, &cand_array[cand_total_cnt], REF_LIST_1);
                            }
                        }
                        INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
                    }
                    context_ptr->injected_mv_x_l1_array[context_ptr->injected_mv_count_l1] =
                        to_inject_mv_x;
                    context_ptr->injected_mv_y_l1_array[context_ptr->injected_mv_count_l1] =
                        to_inject_mv_y;
                    context_ptr->injected_ref_type_l1_array[context_ptr->injected_mv_count_l1] =
                        to_inject_ref_type;
                    ++context_ptr->injected_mv_count_l1;
                }
            }
            /**************
               NEW_NEWMV
            ************* */
            if (allow_bipred) {
                if (inter_direction == 2) {
                    if (!is_valid_bipred_ref(context_ptr, PA_ME_GROUP, me_block_results_ptr->ref0_list, list0_ref_index, me_block_results_ptr->ref1_list, list1_ref_index))
                        continue;
#if !FTR_NEW_REF_PRUNING_CTRLS
                    if (list0_ref_index > context_ptr->md_max_ref_count - 1 ||
                        list1_ref_index > context_ptr->md_max_ref_count - 1)
                        continue;
#endif
                    int16_t to_inject_mv_x_l0 =
                        context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                        [me_block_results_ptr->ref0_list][list0_ref_index][0];
                    int16_t to_inject_mv_y_l0 =
                        context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                        [me_block_results_ptr->ref0_list][list0_ref_index][1];
                    int16_t to_inject_mv_x_l1 =
                        context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                        [me_block_results_ptr->ref1_list][list1_ref_index][0];
                    int16_t to_inject_mv_y_l1 =
                        context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds]
                        [me_block_results_ptr->ref1_list][list1_ref_index][1];
                    uint8_t to_inject_ref_type = av1_ref_frame_type((const MvReferenceFrame[]){
                        svt_get_ref_frame_type(me_block_results_ptr->ref0_list, list0_ref_index),
                        svt_get_ref_frame_type(me_block_results_ptr->ref1_list, list1_ref_index)});

                    inside_tile = 1;
                    if (umv0tile) {
                        inside_tile = is_inside_tile_boundary(&(xd->tile),
                                                              to_inject_mv_x_l0,
                                                              to_inject_mv_y_l0,
                                                              mi_col,
                                                              mi_row,
                                                              context_ptr->blk_geom->bsize) &&
                                      is_inside_tile_boundary(&(xd->tile),
                                                              to_inject_mv_x_l1,
                                                              to_inject_mv_y_l1,
                                                              mi_col,
                                                              mi_row,
                                                              context_ptr->blk_geom->bsize);
                    }
                    uint8_t skip_cand = (!inside_tile);
                    if (!skip_cand &&
                        (context_ptr->injected_mv_count_bipred == 0 ||
                         mrp_is_already_injected_mv_bipred(context_ptr,
                                                           to_inject_mv_x_l0,
                                                           to_inject_mv_y_l0,
                                                           to_inject_mv_x_l1,
                                                           to_inject_mv_y_l1,
                                                           to_inject_ref_type) == EB_FALSE)) {
                        // Set the allowable compound types to be injected
                        uint8_t allowed_comp_types[MD_COMP_TYPES];
                        memcpy(allowed_comp_types, context_ptr->inter_comp_ctrls.allowed_comp_types, sizeof(uint8_t) * MD_COMP_TYPES);
#if !FTR_NEW_REF_PRUNING_CTRLS
                        // Reduce the compound types to inject based on the distance of the reference frames from the current frame
                        set_comp_types_by_distance(allowed_comp_types,
                            context_ptr->inter_comp_ctrls.allowed_dist1_comp_types,
                            context_ptr->inter_comp_ctrls.allowed_dist2_comp_types,
                            list0_ref_index,
                            list1_ref_index);
#endif
                        EbBool mask_done = 0;
#if FTR_UPGRADE_COMP_LEVELS
                        uint8_t drl_index = 0;
                        choose_best_av1_mv_pred(
                            context_ptr,
                            context_ptr->md_rate_estimation_ptr,
                            context_ptr->blk_ptr,
                            to_inject_ref_type,
                            1,
                            NEW_NEWMV,
                            to_inject_mv_x_l0,
                            to_inject_mv_y_l0,
                            to_inject_mv_x_l1,
                            to_inject_mv_y_l1,
                            &drl_index,
                            best_pred_mv);

                        for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < tot_comp_types; cur_type++) {
#else
                        for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < MD_COMP_TYPES; cur_type++) {
#endif

                            // Skip the current compound type if not set to be injected
                            if (allowed_comp_types[cur_type] == 0)
                                continue;
#if FTR_NEW_REF_PRUNING_CTRLS
                            if (!is_valid_bi_type(context_ptr, cur_type, me_block_results_ptr->ref0_list, list0_ref_index, me_block_results_ptr->ref1_list, list1_ref_index))
                                continue;
#endif
                            cand_array[cand_total_cnt].type = INTER_MODE;
#if !CLN_MD_CANDS
                            cand_array[cand_total_cnt].distortion_ready = 0;
#endif
                            cand_array[cand_total_cnt].use_intrabc      = 0;

                            cand_array[cand_total_cnt].skip_mode_allowed = EB_FALSE;
#if !CLN_MD_CANDS
                            cand_array[cand_total_cnt].is_new_mv  = 1;
#endif
#if FTR_UPGRADE_COMP_LEVELS
                            cand_array[cand_total_cnt].drl_index = drl_index;
#else
                            cand_array[cand_total_cnt].drl_index = 0;
#endif

                            // Set the MV to ME result

                            cand_array[cand_total_cnt].motion_vector_xl0 = to_inject_mv_x_l0;
                            cand_array[cand_total_cnt].motion_vector_yl0 = to_inject_mv_y_l0;
                            cand_array[cand_total_cnt].motion_vector_xl1 = to_inject_mv_x_l1;
                            cand_array[cand_total_cnt].motion_vector_yl1 = to_inject_mv_y_l1;

                            // will be needed later by the rate estimation
#if !CLN_MD_CANDS
                            cand_array[cand_total_cnt].ref_mv_index            = 0;
                            cand_array[cand_total_cnt].inter_mode              = NEW_NEWMV;
#endif
                            cand_array[cand_total_cnt].pred_mode               = NEW_NEWMV;
                            cand_array[cand_total_cnt].motion_mode             = SIMPLE_TRANSLATION;
                            cand_array[cand_total_cnt].is_compound             = 1;
                            cand_array[cand_total_cnt].is_interintra_used      = 0;
                            cand_array[cand_total_cnt].prediction_direction[0] = (EbPredDirection)2;
#if CLN_MD_CANDS
                            cand_array[cand_total_cnt].ref_frame_type = to_inject_ref_type;
#else
                            cand_array[cand_total_cnt].ref_frame_type = av1_ref_frame_type(
                                (const MvReferenceFrame[]){
                                    svt_get_ref_frame_type(me_block_results_ptr->ref0_list,
                                                           list0_ref_index),
                                    svt_get_ref_frame_type(me_block_results_ptr->ref1_list,
                                                           list1_ref_index)});

                            cand_array[cand_total_cnt].ref_frame_index_l0 = list0_ref_index;
                            cand_array[cand_total_cnt].ref_frame_index_l1 = list1_ref_index;
                            cand_array[cand_total_cnt].transform_type[0]  = DCT_DCT;
                            cand_array[cand_total_cnt].transform_type_uv  = DCT_DCT;
#endif
#if !FTR_UPGRADE_COMP_LEVELS
                            choose_best_av1_mv_pred(
                                context_ptr,
#if CLN_FAST_COST
                                context_ptr->md_rate_estimation_ptr,
#else
                                cand_array[cand_total_cnt].md_rate_estimation_ptr,
#endif
                                context_ptr->blk_ptr,
                                cand_array[cand_total_cnt].ref_frame_type,
                                cand_array[cand_total_cnt].is_compound,
                                cand_array[cand_total_cnt].pred_mode,
                                cand_array[cand_total_cnt].motion_vector_xl0,
                                cand_array[cand_total_cnt].motion_vector_yl0,
                                cand_array[cand_total_cnt].motion_vector_xl1,
                                cand_array[cand_total_cnt].motion_vector_yl1,
                                &cand_array[cand_total_cnt].drl_index,
                                best_pred_mv);
#endif
                            cand_array[cand_total_cnt].motion_vector_pred_x[REF_LIST_0] =
                                best_pred_mv[0].as_mv.col;
                            cand_array[cand_total_cnt].motion_vector_pred_y[REF_LIST_0] =
                                best_pred_mv[0].as_mv.row;
                            cand_array[cand_total_cnt].motion_vector_pred_x[REF_LIST_1] =
                                best_pred_mv[1].as_mv.col;
                            cand_array[cand_total_cnt].motion_vector_pred_y[REF_LIST_1] =
                                best_pred_mv[1].as_mv.row;
                            //NEW_NEW
#if FTR_UPGRADE_COMP_LEVELS
                            if (cur_type > MD_COMP_AVG) {
                                if (mask_done != 1) {
                                    if (calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_total_cnt]))
                                        break;
                                    mask_done = 1;
                                }
                            }
#else
                            if (cur_type == MD_COMP_DIFF0 || cur_type == MD_COMP_WEDGE) {
                                if (mask_done != 1) {
                                    calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_total_cnt]);
                                    mask_done = 1;
                                }
                            }
#endif
                            determine_compound_mode(
                                pcs_ptr, context_ptr, &cand_array[cand_total_cnt], cur_type);
                            INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
#if !FTR_UPGRADE_COMP_LEVELS
                            context_ptr->injected_mv_x_bipred_l0_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l0;
                            context_ptr->injected_mv_y_bipred_l0_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l0;
                            context_ptr->injected_mv_x_bipred_l1_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l1;
                            context_ptr->injected_mv_y_bipred_l1_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l1;
                            context_ptr->injected_ref_type_bipred_array
                                [context_ptr->injected_mv_count_bipred] = to_inject_ref_type;
                            ++context_ptr->injected_mv_count_bipred;
#endif
                        }
#if FTR_UPGRADE_COMP_LEVELS
                        context_ptr->injected_mv_x_bipred_l0_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l0;
                        context_ptr->injected_mv_y_bipred_l0_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l0;
                        context_ptr->injected_mv_x_bipred_l1_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l1;
                        context_ptr->injected_mv_y_bipred_l1_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l1;
                        context_ptr->injected_ref_type_bipred_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_ref_type;
                        ++context_ptr->injected_mv_count_bipred;
#endif
                    }
                }
            }
        }
    }
    // update the total number of candidates injected
    (*candidate_total_cnt) = cand_total_cnt;
}
void inject_global_candidates(const SequenceControlSet *  scs_ptr,
    struct ModeDecisionContext *context_ptr, PictureControlSet *pcs_ptr,
    EbBool is_compound_enabled, EbBool allow_bipred, uint32_t *candidate_total_cnt) {

    ModeDecisionCandidate *cand_array = context_ptr->fast_candidate_array;
    uint32_t cand_total_cnt = (*candidate_total_cnt);
    uint8_t inj_mv;
    int inside_tile = 1;
    MacroBlockD *xd = context_ptr->blk_ptr->av1xd;
    int umv0tile = (scs_ptr->static_config.unrestricted_motion_vector == 0);
    uint32_t  mi_row = context_ptr->blk_origin_y >> MI_SIZE_LOG2;
    uint32_t  mi_col = context_ptr->blk_origin_x >> MI_SIZE_LOG2;
    BlockSize bsize = context_ptr->blk_geom->bsize; // bloc size

    for (uint32_t ref_it = 0; ref_it < pcs_ptr->parent_pcs_ptr->tot_ref_frame_types; ++ref_it) {

        MvReferenceFrame ref_pair = pcs_ptr->parent_pcs_ptr->ref_frame_type_arr[ref_it];
        MvReferenceFrame rf[2];
        av1_set_ref_frame(rf, ref_pair);

        //single ref/list
        if (rf[1] == NONE_FRAME) {
            if (pcs_ptr->parent_pcs_ptr->gm_ctrls.bipred_only)
                continue;
            MvReferenceFrame frame_type = rf[0];
            uint8_t          list_idx = get_list_idx(rf[0]);
            uint8_t          ref_idx = get_ref_frame_idx(rf[0]);


            if (!is_valid_unipred_ref(context_ptr, GLOBAL_GROUP, list_idx, ref_idx)) continue;
#if !FTR_NEW_REF_PRUNING_CTRLS
            if (ref_idx > context_ptr->md_max_ref_count - 1)
                continue;
#endif
            // Get gm params
            EbWarpedMotionParams *gm_params = &pcs_ptr->parent_pcs_ptr->global_motion[frame_type];

            IntMv mv = gm_get_motion_vector_enc(
                gm_params,
                pcs_ptr->parent_pcs_ptr->frm_hdr.allow_high_precision_mv,
                context_ptr->blk_geom->bsize,
                mi_col,
                mi_row,
                0 /* force_integer_mv */);

            int16_t to_inject_mv_x = mv.as_mv.col;
            int16_t to_inject_mv_y = mv.as_mv.row;

            inj_mv = 1; // Always test GLOBAL even if MV already injected as rate diff might be significant
            if (umv0tile)
                inside_tile = is_inside_tile_boundary(&(xd->tile),
                    to_inject_mv_x,
                    to_inject_mv_y,
                    mi_col,
                    mi_row,
                    context_ptr->blk_geom->bsize);

            inj_mv = inj_mv && inside_tile;

            if (inj_mv && (((gm_params->wmtype > TRANSLATION && context_ptr->blk_geom->bwidth >= 8 && context_ptr->blk_geom->bheight >= 8) || gm_params->wmtype <= TRANSLATION))) {

                uint8_t inter_type;
                uint8_t is_ii_allowed =
                    svt_is_interintra_allowed(context_ptr->inter_intra_comp_ctrls.enabled, bsize, GLOBALMV, rf);
#if !FTR_NEW_REF_PRUNING_CTRLS // intra-inter
                if (is_ii_allowed && context_ptr->inter_intra_comp_ctrls.closest_ref_only)
                    if (ref_idx > 0)
                        is_ii_allowed = 0;
#endif
                uint8_t tot_inter_types = is_ii_allowed ? II_COUNT : 1;

                for (inter_type = 0; inter_type < tot_inter_types; inter_type++) {
#if FTR_NEW_REF_PRUNING_CTRLS
                    if (!is_valid_uni_type(context_ptr, inter_type, is_ii_allowed, 0, list_idx, ref_idx))
                        continue;
#endif
                    cand_array[cand_total_cnt].type = INTER_MODE;
#if !CLN_MD_CANDS
                    cand_array[cand_total_cnt].inter_mode = GLOBALMV;
#endif
                    cand_array[cand_total_cnt].pred_mode = GLOBALMV;
                    cand_array[cand_total_cnt].motion_mode = gm_params->wmtype > TRANSLATION
                        ? WARPED_CAUSAL
                        : SIMPLE_TRANSLATION;

                    cand_array[cand_total_cnt].wm_params_l0 = *gm_params;
                    cand_array[cand_total_cnt].wm_params_l1 = *gm_params;

                    cand_array[cand_total_cnt].is_compound = 0;
#if !CLN_MD_CANDS
                    cand_array[cand_total_cnt].distortion_ready = 0;
#endif
                    cand_array[cand_total_cnt].use_intrabc = 0;
                    cand_array[cand_total_cnt].skip_mode_allowed = EB_FALSE;
                    cand_array[cand_total_cnt].prediction_direction[0] = list_idx;
#if !CLN_MD_CANDS
                    cand_array[cand_total_cnt].is_new_mv = 0;
#endif
                    if (list_idx == 0) {
                        cand_array[cand_total_cnt].motion_vector_xl0 = to_inject_mv_x;
                        cand_array[cand_total_cnt].motion_vector_yl0 = to_inject_mv_y;
                        context_ptr->injected_mv_x_l0_array[context_ptr->injected_mv_count_l0] =
                            to_inject_mv_x;
                        context_ptr->injected_mv_y_l0_array[context_ptr->injected_mv_count_l0] =
                            to_inject_mv_y;
                        context_ptr->injected_ref_type_l0_array[context_ptr->injected_mv_count_l0] =
                            frame_type;
                        ++context_ptr->injected_mv_count_l0;
                    }
                    else {
                        cand_array[cand_total_cnt].motion_vector_xl1 = to_inject_mv_x;
                        cand_array[cand_total_cnt].motion_vector_yl1 = to_inject_mv_y;
                        context_ptr->injected_mv_x_l1_array[context_ptr->injected_mv_count_l1] =
                            to_inject_mv_x;
                        context_ptr->injected_mv_y_l1_array[context_ptr->injected_mv_count_l1] =
                            to_inject_mv_y;
                        context_ptr->injected_ref_type_l1_array[context_ptr->injected_mv_count_l1] =
                            frame_type;
                        ++context_ptr->injected_mv_count_l1;
                    }

                    cand_array[cand_total_cnt].drl_index = 0;
#if !CLN_MD_CANDS
                    cand_array[cand_total_cnt].ref_mv_index = 0;
#endif
                    cand_array[cand_total_cnt].ref_frame_type = frame_type;
#if !CLN_MD_CANDS
                    cand_array[cand_total_cnt].ref_frame_index_l0 = (list_idx == 0) ? ref_idx : -1;
                    cand_array[cand_total_cnt].ref_frame_index_l1 = (list_idx == 1) ? ref_idx : -1;
                    cand_array[cand_total_cnt].transform_type[0] = DCT_DCT;
                    cand_array[cand_total_cnt].transform_type_uv = DCT_DCT;
#endif
                    if (inter_type == 0) {
                        cand_array[cand_total_cnt].is_interintra_used = 0;
                    }
                    else {
                        if (is_ii_allowed) {
                            if (inter_type == 1) {
                                inter_intra_search(
                                    pcs_ptr, context_ptr, &cand_array[cand_total_cnt]);
                                cand_array[cand_total_cnt].is_interintra_used = 1;
                                cand_array[cand_total_cnt].use_wedge_interintra = 1;

                            }
                            else if (inter_type == 2) {
                                cand_array[cand_total_cnt].is_interintra_used = 1;
                                cand_array[cand_total_cnt].interintra_mode =
                                    cand_array[cand_total_cnt - 1].interintra_mode;
                                cand_array[cand_total_cnt].use_wedge_interintra = 0;
                            }
                        }
                    }
                    INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
                }
                if (list_idx == 0) {
                    cand_array[cand_total_cnt].motion_vector_xl0 = to_inject_mv_x;
                    cand_array[cand_total_cnt].motion_vector_yl0 = to_inject_mv_y;
                    context_ptr->injected_mv_x_l0_array[context_ptr->injected_mv_count_l0] =
                        to_inject_mv_x;
                    context_ptr->injected_mv_y_l0_array[context_ptr->injected_mv_count_l0] =
                        to_inject_mv_y;
                    context_ptr->injected_ref_type_l0_array[context_ptr->injected_mv_count_l0] =
                        frame_type;
                    ++context_ptr->injected_mv_count_l0;
                }
                else {
                    cand_array[cand_total_cnt].motion_vector_xl1 = to_inject_mv_x;
                    cand_array[cand_total_cnt].motion_vector_yl1 = to_inject_mv_y;
                    context_ptr->injected_mv_x_l1_array[context_ptr->injected_mv_count_l1] =
                        to_inject_mv_x;
                    context_ptr->injected_mv_y_l1_array[context_ptr->injected_mv_count_l1] =
                        to_inject_mv_y;
                    context_ptr->injected_ref_type_l1_array[context_ptr->injected_mv_count_l1] =
                        frame_type;
                    ++context_ptr->injected_mv_count_l1;
                }
            }
        }
        else if (is_compound_enabled && allow_bipred) {

            uint8_t ref_idx_0 = get_ref_frame_idx(rf[0]);
            uint8_t ref_idx_1 = get_ref_frame_idx(rf[1]);
            uint8_t list_idx_0 = get_list_idx(rf[0]);
            uint8_t list_idx_1 = get_list_idx(rf[1]);

            if (!is_valid_bipred_ref(
                context_ptr, GLOBAL_GROUP, list_idx_0, ref_idx_0, list_idx_1, ref_idx_1)) return;
#if !FTR_NEW_REF_PRUNING_CTRLS
            if (ref_idx_0 > context_ptr->md_max_ref_count - 1 ||
                ref_idx_1 > context_ptr->md_max_ref_count - 1)
                return;
#endif
            // Get gm params
            EbWarpedMotionParams *gm_params_0 =
                &pcs_ptr->parent_pcs_ptr->global_motion[svt_get_ref_frame_type(
                    list_idx_0, ref_idx_0)];

            EbWarpedMotionParams *gm_params_1 =
                &pcs_ptr->parent_pcs_ptr->global_motion[svt_get_ref_frame_type(
                    list_idx_1, ref_idx_1)];

            IntMv mv_0 = gm_get_motion_vector_enc(
                gm_params_0,
                pcs_ptr->parent_pcs_ptr->frm_hdr.allow_high_precision_mv,
                context_ptr->blk_geom->bsize,
                mi_col,
                mi_row,
                0 /* force_integer_mv */);

            int16_t to_inject_mv_x_l0 = mv_0.as_mv.col;
            int16_t to_inject_mv_y_l0 = mv_0.as_mv.row;

            IntMv mv_1 = gm_get_motion_vector_enc(
                gm_params_1,
                pcs_ptr->parent_pcs_ptr->frm_hdr.allow_high_precision_mv,
                context_ptr->blk_geom->bsize,
                mi_col,
                mi_row,
                0 /* force_integer_mv */);

            int16_t to_inject_mv_x_l1 = mv_1.as_mv.col;
            int16_t to_inject_mv_y_l1 = mv_1.as_mv.row;

            inj_mv = 1; // Always test GLOBAL-GLOBAL even if MV already injected as rate diff might be significant
            if (umv0tile) {
                inside_tile = is_inside_tile_boundary(&(xd->tile),
                    to_inject_mv_x_l0,
                    to_inject_mv_y_l0,
                    mi_col,
                    mi_row,
                    context_ptr->blk_geom->bsize) &&
                    is_inside_tile_boundary(&(xd->tile),
                        to_inject_mv_x_l1,
                        to_inject_mv_y_l1,
                        mi_col,
                        mi_row,
                        context_ptr->blk_geom->bsize);
            }

            inj_mv = inj_mv && inside_tile;

            if (inj_mv && gm_params_0->wmtype > TRANSLATION && gm_params_1->wmtype > TRANSLATION) {
                uint8_t to_inject_ref_type = av1_ref_frame_type(rf);

                // Set the allowable compound types to be injected
                uint8_t allowed_comp_types[MD_COMP_TYPES];
                memcpy(allowed_comp_types, context_ptr->inter_comp_ctrls.allowed_comp_types, sizeof(uint8_t) * MD_COMP_TYPES);
#if !FTR_NEW_REF_PRUNING_CTRLS
                // Reduce the compound types to inject based on the distance of the reference frames from the current frame
                set_comp_types_by_distance(allowed_comp_types,
                    context_ptr->inter_comp_ctrls.allowed_dist1_comp_types,
                    context_ptr->inter_comp_ctrls.allowed_dist2_comp_types,
                    ref_idx_0,
                    ref_idx_1);
#endif
                // Warped prediction is only compatible with MD_COMP_AVG and MD_COMP_DIST
                for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < MD_COMP_DIFF0; cur_type++) {

                    // Skip the current compound type if not set to be injected
                    if (allowed_comp_types[cur_type] == 0 || cur_type > MD_COMP_DIST)
                        continue;
#if FTR_NEW_REF_PRUNING_CTRLS
                    if (!is_valid_bi_type(context_ptr, cur_type, list_idx_0, ref_idx_0, list_idx_1, ref_idx_1))
                        continue;
#endif
                    cand_array[cand_total_cnt].type = INTER_MODE;
#if !CLN_MD_CANDS
                    cand_array[cand_total_cnt].distortion_ready = 0;
#endif
                    cand_array[cand_total_cnt].use_intrabc = 0;

                    cand_array[cand_total_cnt].skip_mode_allowed = EB_FALSE;

                    cand_array[cand_total_cnt].prediction_direction[0] = BI_PRED;
#if !CLN_MD_CANDS
                    cand_array[cand_total_cnt].inter_mode = GLOBAL_GLOBALMV;
#endif
                    cand_array[cand_total_cnt].pred_mode = GLOBAL_GLOBALMV;
                    cand_array[cand_total_cnt].motion_mode =
                        gm_params_0->wmtype > TRANSLATION ? WARPED_CAUSAL
                        : SIMPLE_TRANSLATION;
                    cand_array[cand_total_cnt].wm_params_l0 = *gm_params_0;
                    cand_array[cand_total_cnt].wm_params_l1 = *gm_params_1;
                    cand_array[cand_total_cnt].is_compound = 1;
                    cand_array[cand_total_cnt].is_interintra_used = 0;
#if !CLN_MD_CANDS
                    cand_array[cand_total_cnt].is_new_mv = 0;
#endif
                    cand_array[cand_total_cnt].drl_index = 0;
                    // will be needed later by the rate estimation
#if !CLN_MD_CANDS
                    cand_array[cand_total_cnt].ref_mv_index = 0;
#endif
                    cand_array[cand_total_cnt].ref_frame_type = to_inject_ref_type;
#if !CLN_MD_CANDS
                    cand_array[cand_total_cnt].ref_frame_index_l0 = ref_idx_0;
                    cand_array[cand_total_cnt].ref_frame_index_l1 = ref_idx_1;
                    cand_array[cand_total_cnt].transform_type[0] = DCT_DCT;
                    cand_array[cand_total_cnt].transform_type_uv = DCT_DCT;
#endif
                    // Set the MV to frame MV

                    cand_array[cand_total_cnt].motion_vector_xl0 =
                        to_inject_mv_x_l0;
                    cand_array[cand_total_cnt].motion_vector_yl0 =
                        to_inject_mv_y_l0;
                    cand_array[cand_total_cnt].motion_vector_xl1 =
                        to_inject_mv_x_l1;
                    cand_array[cand_total_cnt].motion_vector_yl1 =
                        to_inject_mv_y_l1;
                    //GLOB-GLOB
                    determine_compound_mode(pcs_ptr,
                        context_ptr,
                        &cand_array[cand_total_cnt],
                        cur_type);
                    INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
                }
                context_ptr->injected_mv_x_bipred_l0_array
                    [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l0;
                context_ptr->injected_mv_y_bipred_l0_array
                    [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l0;
                context_ptr->injected_mv_x_bipred_l1_array
                    [context_ptr->injected_mv_count_bipred] = to_inject_mv_x_l1;
                context_ptr->injected_mv_y_bipred_l1_array
                    [context_ptr->injected_mv_count_bipred] = to_inject_mv_y_l1;
                context_ptr->injected_ref_type_bipred_array
                    [context_ptr->injected_mv_count_bipred] =
                    to_inject_ref_type;
                ++context_ptr->injected_mv_count_bipred;
            }
        }
    }
    // update the total number of candidates injected
    (*candidate_total_cnt) = cand_total_cnt;
}
#if !FTR_NEW_REF_PRUNING_CTRLS // intra-inter
uint8_t is_reference_best_pme(ModeDecisionContext *context_ptr, uint8_t list_index,
    uint8_t ref_index, uint8_t best_x_reference){

    ASSERT(best_x_reference < MAX_NUM_OF_REF_PIC_LIST*REF_LIST_MAX_DEPTH);

    for (int i = 0; i < best_x_reference; i++)
    {
        if (context_ptr->pme_res[0][i].ref_i == ref_index &&
            context_ptr->pme_res[0][i].list_i == list_index)
            return 1;
    }
    return 0;
}
#endif
void inject_pme_candidates(
    //const SequenceControlSet   *scs_ptr,
    struct ModeDecisionContext *context_ptr, PictureControlSet *pcs_ptr, EbBool is_compound_enabled,
    EbBool allow_bipred, uint32_t *candidate_total_cnt) {
    ModeDecisionCandidate *cand_array      = context_ptr->fast_candidate_array;
    IntMv                  best_pred_mv[2] = {{0}, {0}};
    uint32_t               cand_total_cnt  = (*candidate_total_cnt);
    BlockSize              bsize           = context_ptr->blk_geom->bsize; // bloc size
#if FTR_UPGRADE_COMP_LEVELS
    MD_COMP_TYPE tot_comp_types =
        (context_ptr->inter_comp_ctrls.do_pme == 0)
        ? MD_COMP_DIST
        : MD_COMP_TYPES;
#endif
    Mv mv;
    MvUnit mv_unit;
    for (uint32_t ref_it = 0; ref_it < pcs_ptr->parent_pcs_ptr->tot_ref_frame_types; ++ref_it) {
        MvReferenceFrame ref_pair = pcs_ptr->parent_pcs_ptr->ref_frame_type_arr[ref_it];
        MvReferenceFrame rf[2];
        av1_set_ref_frame(rf, ref_pair);

        //single ref/list
        if (rf[1] == NONE_FRAME) {
            MvReferenceFrame frame_type = rf[0];
            uint8_t          list_idx = get_list_idx(rf[0]);
            uint8_t          ref_idx = get_ref_frame_idx(rf[0]);

            if (context_ptr->valid_pme_mv[list_idx][ref_idx]) {
                int16_t to_inject_mv_x = context_ptr->best_pme_mv[list_idx][ref_idx][0];
                int16_t to_inject_mv_y = context_ptr->best_pme_mv[list_idx][ref_idx][1];

                uint8_t inj_mv =
                    list_idx == 0
                    ? context_ptr->injected_mv_count_l0 == 0 ||
                    mrp_is_already_injected_mv_l0(
                        context_ptr, to_inject_mv_x, to_inject_mv_y, frame_type) ==
                    EB_FALSE
                    : context_ptr->injected_mv_count_l1 == 0 ||
                    mrp_is_already_injected_mv_l1(
                        context_ptr, to_inject_mv_x, to_inject_mv_y, frame_type) ==
                    EB_FALSE;

                if (inj_mv) {
                    uint8_t inter_type;
#if FTR_NEW_REF_PRUNING_CTRLS // pme inter_intra
                    uint8_t is_ii_allowed = svt_is_interintra_allowed(context_ptr->inter_intra_comp_ctrls.enabled, bsize, NEWMV, rf);
#else
                    uint8_t is_ii_allowed = context_ptr->inter_intra_comp_ctrls.enabled ?
                        svt_is_interintra_allowed(!context_ptr->inter_intra_comp_ctrls.skip_pme_unipred, bsize, NEWMV, rf) :
                        0;

                    if (is_ii_allowed && context_ptr->inter_intra_comp_ctrls.closest_ref_only)
                        if (is_reference_best_pme(context_ptr, list_idx, ref_idx, 1) == 0)
                            is_ii_allowed = 0;
#endif
                    uint8_t tot_inter_types = is_ii_allowed ? II_COUNT : 1;
                    uint8_t is_obmc_allowed =
                        obmc_motion_mode_allowed(
                            pcs_ptr, context_ptr, bsize, rf[0], rf[1], NEWMV) == OBMC_CAUSAL;
#if !FTR_NEW_REF_PRUNING_CTRLS // obmc
                    if (context_ptr->obmc_ctrls.pme_best_ref)
                        if (context_ptr->pme_res[0][0].list_i != list_idx ||
                            context_ptr->pme_res[0][0].ref_i != ref_idx)
                            is_obmc_allowed = 0;
#endif
                    uint8_t is_warp_allowed = warped_motion_mode_allowed(pcs_ptr, context_ptr);
                    tot_inter_types = is_warp_allowed ? tot_inter_types + 1 : tot_inter_types;
                    tot_inter_types = is_obmc_allowed ? tot_inter_types + 1 : tot_inter_types;
#if FTR_UPGRADE_COMP_LEVELS
                    uint8_t drl_index = 0;
                    choose_best_av1_mv_pred(context_ptr,
                        context_ptr->md_rate_estimation_ptr,
                        context_ptr->blk_ptr,
                        frame_type,
                        0,
                        NEWMV,
                        to_inject_mv_x,
                        to_inject_mv_y,
                        0,
                        0,
                        &drl_index,
                        best_pred_mv);
#endif
                    for (inter_type = 0; inter_type < tot_inter_types; inter_type++) {
#if FTR_NEW_REF_PRUNING_CTRLS
                        if (!is_valid_uni_type(context_ptr, inter_type, is_ii_allowed, is_warp_allowed, list_idx, ref_idx))
                            continue;
#endif
                        cand_array[cand_total_cnt].type = INTER_MODE;
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].distortion_ready = 0;
#endif
                        cand_array[cand_total_cnt].use_intrabc = 0;
                        cand_array[cand_total_cnt].skip_mode_allowed = EB_FALSE;
                        cand_array[cand_total_cnt].prediction_direction[0] = list_idx;
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].inter_mode = NEWMV;
#endif
                        cand_array[cand_total_cnt].pred_mode = NEWMV;
                        cand_array[cand_total_cnt].motion_mode = SIMPLE_TRANSLATION;
                        cand_array[cand_total_cnt].is_compound = 0;
                        cand_array[cand_total_cnt].is_interintra_used = 0;
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].is_new_mv = 1;
#endif
#if FTR_UPGRADE_COMP_LEVELS
                        cand_array[cand_total_cnt].drl_index = drl_index;
#else
                        cand_array[cand_total_cnt].drl_index = 0;
#endif
                        if (list_idx == 0) {
                            cand_array[cand_total_cnt].motion_vector_xl0 = to_inject_mv_x;
                            cand_array[cand_total_cnt].motion_vector_yl0 = to_inject_mv_y;
                            context_ptr->injected_mv_x_l0_array[context_ptr->injected_mv_count_l0] =
                                to_inject_mv_x;
                            context_ptr->injected_mv_y_l0_array[context_ptr->injected_mv_count_l0] =
                                to_inject_mv_y;
                            context_ptr
                                ->injected_ref_type_l0_array[context_ptr->injected_mv_count_l0] =
                                frame_type;
                            ++context_ptr->injected_mv_count_l0;
                        }
                        else {
                            cand_array[cand_total_cnt].motion_vector_xl1 = to_inject_mv_x;
                            cand_array[cand_total_cnt].motion_vector_yl1 = to_inject_mv_y;
                            context_ptr->injected_mv_x_l1_array[context_ptr->injected_mv_count_l1] =
                                to_inject_mv_x;
                            context_ptr->injected_mv_y_l1_array[context_ptr->injected_mv_count_l1] =
                                to_inject_mv_y;
                            context_ptr
                                ->injected_ref_type_l1_array[context_ptr->injected_mv_count_l1] =
                                frame_type;
                            ++context_ptr->injected_mv_count_l1;
                        }
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].ref_mv_index = 0;
#endif
                        cand_array[cand_total_cnt].ref_frame_type = frame_type;
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].ref_frame_index_l0 =
                            (list_idx == 0) ? ref_idx : -1;
                        cand_array[cand_total_cnt].ref_frame_index_l1 =
                            (list_idx == 1) ? ref_idx : -1;
                        cand_array[cand_total_cnt].transform_type[0] = DCT_DCT;
                        cand_array[cand_total_cnt].transform_type_uv = DCT_DCT;
#endif
#if !FTR_UPGRADE_COMP_LEVELS
                        choose_best_av1_mv_pred(
                            context_ptr,
#if CLN_FAST_COST
                            context_ptr->md_rate_estimation_ptr,
#else
                            cand_array[cand_total_cnt].md_rate_estimation_ptr,
#endif
                            context_ptr->blk_ptr,
                            cand_array[cand_total_cnt].ref_frame_type,
                            cand_array[cand_total_cnt].is_compound,
                            cand_array[cand_total_cnt].pred_mode,
                            list_idx == 0 ? cand_array[cand_total_cnt].motion_vector_xl0
                            : cand_array[cand_total_cnt].motion_vector_xl1,
                            list_idx == 0 ? cand_array[cand_total_cnt].motion_vector_yl0
                            : cand_array[cand_total_cnt].motion_vector_yl1,
                            0,
                            0,
                            &cand_array[cand_total_cnt].drl_index,
                            best_pred_mv);
#endif
                        cand_array[cand_total_cnt].motion_vector_pred_x[list_idx] =
                            best_pred_mv[0].as_mv.col;
                        cand_array[cand_total_cnt].motion_vector_pred_y[list_idx] =
                            best_pred_mv[0].as_mv.row;
                        if (inter_type == 0) {
                            cand_array[cand_total_cnt].is_interintra_used = 0;
                            cand_array[cand_total_cnt].motion_mode = SIMPLE_TRANSLATION;
                        }
                        else {
                            if (is_ii_allowed) {
                                if (inter_type == 1) {
                                    inter_intra_search(
                                        pcs_ptr, context_ptr, &cand_array[cand_total_cnt]);
                                    cand_array[cand_total_cnt].is_interintra_used = 1;
                                    cand_array[cand_total_cnt].use_wedge_interintra = 1;

                                }
                                else if (inter_type == 2) {
                                    cand_array[cand_total_cnt].is_interintra_used = 1;
                                    cand_array[cand_total_cnt].interintra_mode =
                                        cand_array[cand_total_cnt - 1].interintra_mode;
                                    cand_array[cand_total_cnt].use_wedge_interintra = 0;
                                }
                            }
                            if (is_warp_allowed && inter_type == (tot_inter_types - (1 + is_obmc_allowed))) {
                                cand_array[cand_total_cnt].is_interintra_used = 0;
                                cand_array[cand_total_cnt].motion_mode = WARPED_CAUSAL;
                                cand_array[cand_total_cnt].wm_params_l0.wmtype = AFFINE;

                                mv.x = to_inject_mv_x;
                                mv.y = to_inject_mv_y;
                                mv_unit.mv[list_idx] = mv;
                                mv_unit.pred_direction = cand_array[cand_total_cnt].prediction_direction[0];
                                cand_array[cand_total_cnt].local_warp_valid = warped_motion_parameters(
                                    pcs_ptr,
                                    context_ptr->blk_ptr,
                                    &mv_unit,
                                    context_ptr->blk_geom,
                                    context_ptr->blk_origin_x,
                                    context_ptr->blk_origin_y,
                                    cand_array[cand_total_cnt].ref_frame_type,
                                    &cand_array[cand_total_cnt].wm_params_l0,
                                    &cand_array[cand_total_cnt].num_proj_ref);
                            }
                            if (is_obmc_allowed && inter_type == tot_inter_types - 1) {
                                cand_array[cand_total_cnt].is_interintra_used = 0;
                                cand_array[cand_total_cnt].motion_mode = OBMC_CAUSAL;

                                obmc_motion_refinement(
                                    pcs_ptr, context_ptr, &cand_array[cand_total_cnt], list_idx);
                            }
                        }
                        if (!(is_warp_allowed && inter_type == (tot_inter_types - (1 + is_obmc_allowed))))
                            INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
                        else if (cand_array[cand_total_cnt].local_warp_valid)
                            INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
                    }
                }
            }
        }

        else if (is_compound_enabled && allow_bipred) {
            uint8_t ref_idx_0 = get_ref_frame_idx(rf[0]);
            uint8_t ref_idx_1 = get_ref_frame_idx(rf[1]);
            uint8_t list_idx_0 = get_list_idx(rf[0]);
            uint8_t list_idx_1 = get_list_idx(rf[1]);

            if (context_ptr->valid_pme_mv[list_idx_0][ref_idx_0] &&
                context_ptr->valid_pme_mv[list_idx_1][ref_idx_1]) {

                int16_t to_inject_mv_x_l0 =
                    context_ptr->best_pme_mv[list_idx_0][ref_idx_0][0];
                int16_t to_inject_mv_y_l0 =
                    context_ptr->best_pme_mv[list_idx_0][ref_idx_0][1];
                int16_t to_inject_mv_x_l1 =
                    context_ptr->best_pme_mv[list_idx_1][ref_idx_1][0];
                int16_t to_inject_mv_y_l1 =
                    context_ptr->best_pme_mv[list_idx_1][ref_idx_1][1];

                uint8_t to_inject_ref_type = av1_ref_frame_type((const MvReferenceFrame[]){
                    svt_get_ref_frame_type(list_idx_0, ref_idx_0),
                    svt_get_ref_frame_type(list_idx_1, ref_idx_1),
                });
                if (context_ptr->injected_mv_count_bipred == 0 ||
                    mrp_is_already_injected_mv_bipred(context_ptr,
                        to_inject_mv_x_l0,
                        to_inject_mv_y_l0,
                        to_inject_mv_x_l1,
                        to_inject_mv_y_l1,
                        to_inject_ref_type) == EB_FALSE) {
                    // Set the allowable compound types to be injected
                    uint8_t allowed_comp_types[MD_COMP_TYPES];
                    memcpy(allowed_comp_types, context_ptr->inter_comp_ctrls.allowed_comp_types, sizeof(uint8_t) * MD_COMP_TYPES);
#if !FTR_NEW_REF_PRUNING_CTRLS
                    // Reduce the compound types to inject based on the distance of the reference frames from the current frame
                    set_comp_types_by_distance(allowed_comp_types,
                        context_ptr->inter_comp_ctrls.allowed_dist1_comp_types,
                        context_ptr->inter_comp_ctrls.allowed_dist2_comp_types,
                        ref_idx_0,
                        ref_idx_1);
#endif
                    EbBool mask_done = 0;
#if FTR_UPGRADE_COMP_LEVELS
                    uint8_t drl_index = 0;
                    choose_best_av1_mv_pred(
                        context_ptr,
                        context_ptr->md_rate_estimation_ptr,
                        context_ptr->blk_ptr,
                        to_inject_ref_type,
                        1,
                        NEW_NEWMV,
                        to_inject_mv_x_l0,
                        to_inject_mv_y_l0,
                        to_inject_mv_x_l1,
                        to_inject_mv_y_l1,
                        &drl_index,
                        best_pred_mv);

                    for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < tot_comp_types; cur_type++) {
#else
                    for (MD_COMP_TYPE cur_type = MD_COMP_AVG; cur_type < MD_COMP_TYPES; cur_type++) {
#endif

                        // Skip the current compound type if not set to be injected
                        if (allowed_comp_types[cur_type] == 0)
                            continue;
#if FTR_NEW_REF_PRUNING_CTRLS
                        if (!is_valid_bi_type(context_ptr, cur_type, list_idx_0, ref_idx_0, list_idx_1, ref_idx_1))
                            continue;
#endif
                        cand_array[cand_total_cnt].type = INTER_MODE;
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].distortion_ready = 0;
#endif
                        cand_array[cand_total_cnt].use_intrabc = 0;
                        cand_array[cand_total_cnt].skip_mode_allowed = EB_FALSE;
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].is_new_mv = 1;
#endif
#if FTR_UPGRADE_COMP_LEVELS
                        cand_array[cand_total_cnt].drl_index = drl_index;
#else
                        cand_array[cand_total_cnt].drl_index = 0;
#endif
                        // Set the MV to ME result
                        cand_array[cand_total_cnt].motion_vector_xl0 = to_inject_mv_x_l0;
                        cand_array[cand_total_cnt].motion_vector_yl0 = to_inject_mv_y_l0;
                        cand_array[cand_total_cnt].motion_vector_xl1 = to_inject_mv_x_l1;
                        cand_array[cand_total_cnt].motion_vector_yl1 = to_inject_mv_y_l1;
                        // will be needed later by the rate estimation
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].ref_mv_index = 0;
                        cand_array[cand_total_cnt].inter_mode = NEW_NEWMV;
#endif
                        cand_array[cand_total_cnt].pred_mode = NEW_NEWMV;
                        cand_array[cand_total_cnt].motion_mode = SIMPLE_TRANSLATION;
                        cand_array[cand_total_cnt].is_compound = 1;
                        cand_array[cand_total_cnt].is_interintra_used = 0;
                        cand_array[cand_total_cnt].prediction_direction[0] = BI_PRED;
#if CLN_MD_CANDS
                        cand_array[cand_total_cnt].ref_frame_type = to_inject_ref_type;
#else
                        cand_array[cand_total_cnt].ref_frame_type = av1_ref_frame_type((const MvReferenceFrame[]){
                            svt_get_ref_frame_type(list_idx_0, ref_idx_0),
                            svt_get_ref_frame_type(list_idx_1, ref_idx_1),
                        });

                        cand_array[cand_total_cnt].ref_frame_index_l0 = ref_idx_0;
                        cand_array[cand_total_cnt].ref_frame_index_l1 = ref_idx_1;
                        cand_array[cand_total_cnt].transform_type[0] = DCT_DCT;
                        cand_array[cand_total_cnt].transform_type_uv = DCT_DCT;
#endif
#if !FTR_UPGRADE_COMP_LEVELS
                        choose_best_av1_mv_pred(context_ptr,
#if CLN_FAST_COST
                            context_ptr->md_rate_estimation_ptr,
#else
                            cand_array[cand_total_cnt].md_rate_estimation_ptr,
#endif
                            context_ptr->blk_ptr,
                            cand_array[cand_total_cnt].ref_frame_type,
                            cand_array[cand_total_cnt].is_compound,
                            cand_array[cand_total_cnt].pred_mode,
                            cand_array[cand_total_cnt].motion_vector_xl0,
                            cand_array[cand_total_cnt].motion_vector_yl0,
                            cand_array[cand_total_cnt].motion_vector_xl1,
                            cand_array[cand_total_cnt].motion_vector_yl1,
                            &cand_array[cand_total_cnt].drl_index,
                            best_pred_mv);
#endif
                        cand_array[cand_total_cnt].motion_vector_pred_x[REF_LIST_0] =
                            best_pred_mv[0].as_mv.col;
                        cand_array[cand_total_cnt].motion_vector_pred_y[REF_LIST_0] =
                            best_pred_mv[0].as_mv.row;
                        cand_array[cand_total_cnt].motion_vector_pred_x[REF_LIST_1] =
                            best_pred_mv[1].as_mv.col;
                        cand_array[cand_total_cnt].motion_vector_pred_y[REF_LIST_1] =
                            best_pred_mv[1].as_mv.row;

                        //MVP REFINE
#if FTR_UPGRADE_COMP_LEVELS
                        if (cur_type > MD_COMP_AVG) {
                            if (mask_done != 1) {
                                if (calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_total_cnt]))
                                    break;
                                mask_done = 1;
                            }
                        }
#else
                        if (cur_type == MD_COMP_DIFF0 || cur_type == MD_COMP_WEDGE) {
                            if (mask_done != 1) {
                                calc_pred_masked_compound(pcs_ptr, context_ptr, &cand_array[cand_total_cnt]);
                                mask_done = 1;
                            }
                        }
#endif
                        determine_compound_mode(
                            pcs_ptr, context_ptr, &cand_array[cand_total_cnt], cur_type);
                        INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
#if !FTR_UPGRADE_COMP_LEVELS
                        context_ptr
                            ->injected_mv_x_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_x_l0;
                        context_ptr
                            ->injected_mv_y_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_y_l0;
                        context_ptr
                            ->injected_mv_x_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_x_l1;
                        context_ptr
                            ->injected_mv_y_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                            to_inject_mv_y_l1;

                        context_ptr->injected_ref_type_bipred_array
                            [context_ptr->injected_mv_count_bipred] = to_inject_ref_type;
                        ++context_ptr->injected_mv_count_bipred;
#endif
                    }
#if FTR_UPGRADE_COMP_LEVELS
                    context_ptr
                        ->injected_mv_x_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                        to_inject_mv_x_l0;
                    context_ptr
                        ->injected_mv_y_bipred_l0_array[context_ptr->injected_mv_count_bipred] =
                        to_inject_mv_y_l0;
                    context_ptr
                        ->injected_mv_x_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                        to_inject_mv_x_l1;
                    context_ptr
                        ->injected_mv_y_bipred_l1_array[context_ptr->injected_mv_count_bipred] =
                        to_inject_mv_y_l1;
                    context_ptr->injected_ref_type_bipred_array
                        [context_ptr->injected_mv_count_bipred] = to_inject_ref_type;
                    ++context_ptr->injected_mv_count_bipred;
#endif
                }
            }
        }
    }
    (*candidate_total_cnt) = cand_total_cnt;
}

void inject_inter_candidates(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                             const SequenceControlSet *scs_ptr, SuperBlock *sb_ptr, uint32_t *candidate_total_cnt) {
    (void)scs_ptr;

    FrameHeader *          frm_hdr        = &pcs_ptr->parent_pcs_ptr->frm_hdr;
    uint32_t               cand_total_cnt = *candidate_total_cnt;
    EbBool       is_compound_enabled      = (frm_hdr->reference_mode == SINGLE_REFERENCE) ? 0 : 1;
    MeSbResults *me_results =
        pcs_ptr->parent_pcs_ptr->pa_me_data->me_results[context_ptr->me_sb_addr];
    EbBool       allow_bipred =
        (context_ptr->blk_geom->bwidth == 4 || context_ptr->blk_geom->bheight == 4)
        ? EB_FALSE : EB_TRUE;
    uint32_t mi_row = context_ptr->blk_origin_y >> MI_SIZE_LOG2;
    uint32_t mi_col = context_ptr->blk_origin_x >> MI_SIZE_LOG2;

    svt_av1_count_overlappable_neighbors(
        pcs_ptr, context_ptr->blk_ptr, context_ptr->blk_geom->bsize, mi_row, mi_col);
    const uint8_t is_obmc_allowed = obmc_motion_mode_allowed(pcs_ptr,
                                                             context_ptr,
                                                             context_ptr->blk_geom->bsize,
                                                             LAST_FRAME,
                                                             -1,
                                                             NEWMV) == OBMC_CAUSAL;
    if (is_obmc_allowed)
        precompute_obmc_data(pcs_ptr, context_ptr);
    /**************
         MVP
    ************* */
    if (context_ptr->new_nearest_injection)
#if CLN_MD_CANDS
        inject_mvp_candidates_ii(scs_ptr, pcs_ptr, context_ptr, &cand_total_cnt);
#else
        //all of ref pairs: (1)single-ref List0  (2)single-ref List1  (3)compound Bi-Dir List0-List1  (4)compound Uni-Dir List0-List0  (5)compound Uni-Dir List1-List1
        for (uint32_t ref_it = 0; ref_it < pcs_ptr->parent_pcs_ptr->tot_ref_frame_types; ++ref_it) {
            const MvReferenceFrame ref_frame_pair =
                pcs_ptr->parent_pcs_ptr->ref_frame_type_arr[ref_it];
            inject_mvp_candidates_ii(
                context_ptr, pcs_ptr, context_ptr->blk_ptr, ref_frame_pair, &cand_total_cnt);
        }
#endif
    //----------------------
    //    NEAREST_NEWMV, NEW_NEARESTMV, NEAR_NEWMV, NEW_NEARMV.
    //----------------------
    if (context_ptr->new_nearest_near_comb_injection) {
        const EbBool allow_compound = frm_hdr->reference_mode != SINGLE_REFERENCE &&
            context_ptr->blk_geom->bwidth != 4 && context_ptr->blk_geom->bheight != 4;
        if (allow_compound) {
#if CLN_MD_CANDS
            inject_new_nearest_new_comb_candidates(scs_ptr, pcs_ptr, context_ptr, &cand_total_cnt);
#else
            //all of ref pairs: (1)single-ref List0  (2)single-ref List1  (3)compound Bi-Dir List0-List1  (4)compound Uni-Dir List0-List0  (5)compound Uni-Dir List1-List1
            for (uint32_t ref_it = 0; ref_it < pcs_ptr->parent_pcs_ptr->tot_ref_frame_types;
                 ++ref_it) {
                const MvReferenceFrame ref_frame_pair =
                    pcs_ptr->parent_pcs_ptr->ref_frame_type_arr[ref_it];
                inject_new_nearest_new_comb_candidates(
                    scs_ptr, context_ptr, pcs_ptr, ref_frame_pair, &cand_total_cnt);
            }
#endif
        }
    }
#if FTR_PD2_REDUCE_MDS0
    if (context_ptr->inject_new_me)
#endif
    inject_new_candidates(scs_ptr,
                          context_ptr,
                          pcs_ptr,
                          is_compound_enabled,
                          allow_bipred,
                          context_ptr->me_sb_addr,
                          context_ptr->me_block_offset,
                          &cand_total_cnt);
    if (context_ptr->global_mv_injection) {
        inject_global_candidates(scs_ptr,
            context_ptr,
            pcs_ptr,
            is_compound_enabled,
            allow_bipred,
            &cand_total_cnt);
    }

    // Warped Motion
#if FTR_PD2_REDUCE_MDS0
    if (context_ptr->inject_new_warp) {
#endif
        if (warped_motion_mode_allowed(pcs_ptr, context_ptr)) {
            inject_warped_motion_candidates(
                pcs_ptr, context_ptr, context_ptr->blk_ptr, &cand_total_cnt, me_results);
        }
#if FTR_PD2_REDUCE_MDS0
    }
#endif
    if (is_compound_enabled) {
        if (allow_bipred && context_ptr->bipred3x3_injection > 0 && pcs_ptr->slice_type == B_SLICE)
            //----------------------
            // Bipred2Nx2N
            //----------------------
            bipred_3x3_candidates_injection(
                scs_ptr, pcs_ptr, context_ptr, sb_ptr, context_ptr->me_sb_addr, &cand_total_cnt);

        //----------------------
        // Unipred2Nx2N
        //----------------------
        if (context_ptr->unipred3x3_injection > 0 && pcs_ptr->slice_type != I_SLICE)
            unipred_3x3_candidates_injection(
                scs_ptr, pcs_ptr, context_ptr, sb_ptr, context_ptr->me_sb_addr, &cand_total_cnt);
    }
#if FTR_PD2_REDUCE_MDS0
    if (context_ptr->inject_new_pme && context_ptr->md_pme_ctrls.enabled)
#else
    if (context_ptr->md_pme_ctrls.enabled)
#endif
        inject_pme_candidates(
            context_ptr, pcs_ptr, is_compound_enabled, allow_bipred, &cand_total_cnt);
    // update the total number of candidates injected
    *candidate_total_cnt = cand_total_cnt;
}

#if RFCTR_INTRA_TX_INIT_FUNC
static INLINE TxType av1_get_tx_type(int32_t is_inter, PredictionMode pred_mode,
    UvPredictionMode pred_mode_uv, PlaneType plane_type,
    TxSize tx_size, int32_t reduced_tx_set) {

    if (txsize_sqr_up_map[tx_size] > TX_32X32 || plane_type == PLANE_TYPE_Y || is_inter) {
        return DCT_DCT;
    }

    MbModeInfo mbmi;
    mbmi.block_mi.mode = pred_mode;
    mbmi.block_mi.uv_mode = pred_mode_uv;

    // In intra mode, uv planes don't share the same prediction mode as y
    // plane, so the tx_type should not be shared
    TxType tx_type = intra_mode_to_tx_type(&mbmi.block_mi, PLANE_TYPE_UV);

    assert(tx_type < TX_TYPES);
    const TxSetType tx_set_type = get_ext_tx_set_type(tx_size, is_inter, reduced_tx_set);
    return !av1_ext_tx_used[tx_set_type][tx_type] ? DCT_DCT : tx_type;
}
#else
static INLINE TxType av1_get_tx_type(BlockSize sb_type, int32_t is_inter, PredictionMode pred_mode,
                                     UvPredictionMode pred_mode_uv, PlaneType plane_type,
                                     const MacroBlockD *xd, int32_t blk_row, int32_t blk_col,
                                     TxSize tx_size, int32_t reduced_tx_set) {
    UNUSED(sb_type);
    UNUSED(xd);
    UNUSED(blk_row);
    UNUSED(blk_col);

    // BlockSize  sb_type = BLOCK_8X8;

    MbModeInfo mbmi;
    mbmi.block_mi.mode    = pred_mode;
    mbmi.block_mi.uv_mode = pred_mode_uv;

    // const MbModeInfo *const mbmi = xd->mi[0];
    // const struct MacroblockdPlane *const pd = &xd->plane[plane_type];
    const TxSetType tx_set_type =
        /*av1_*/ get_ext_tx_set_type(tx_size, is_inter, reduced_tx_set);

    TxType tx_type = DCT_DCT;
    if (/*xd->lossless[mbmi->segment_id] ||*/ txsize_sqr_up_map[tx_size] > TX_32X32)
        tx_type = DCT_DCT;
    else {
        if (plane_type == PLANE_TYPE_Y) {
            //const int32_t txk_type_idx =
            //    av1_get_txk_type_index(/*mbmi->*/sb_type, blk_row, blk_col);
            //tx_type = mbmi->txk_type[txk_type_idx];
        } else if (is_inter /*is_inter_block(mbmi)*/) {
            // scale back to y plane's coordinate
            //blk_row <<= pd->subsampling_y;
            //blk_col <<= pd->subsampling_x;
            //const int32_t txk_type_idx =
            //    av1_get_txk_type_index(mbmi->sb_type, blk_row, blk_col);
            //tx_type = mbmi->txk_type[txk_type_idx];
        } else {
            // In intra mode, uv planes don't share the same prediction mode as y
            // plane, so the tx_type should not be shared
            tx_type = intra_mode_to_tx_type(&mbmi.block_mi, PLANE_TYPE_UV);
        }
    }
    ASSERT(tx_type < TX_TYPES);
    if (!av1_ext_tx_used[tx_set_type][tx_type]) return DCT_DCT;
    return tx_type;
}
#endif
double svt_av1_convert_qindex_to_q(int32_t qindex, AomBitDepth bit_depth);

// Values are now correlated to quantizer.
static int sad_per_bit16lut_8[QINDEX_RANGE];
static int sad_per_bit_lut_10[QINDEX_RANGE];
static void init_me_luts_bd(int *bit16lut, int range,
    AomBitDepth bit_depth) {
    int i;
    // Initialize the sad lut tables using a formulaic calculation for now.
    // This is to make it easier to resolve the impact of experimental changes
    // to the quantizer tables.
    for (i = 0; i < range; i++) {
        const double q = svt_av1_convert_qindex_to_q(i, bit_depth);
        bit16lut[i] = (int)(0.0418 * q + 2.4107);
    }
}
void svt_av1_init_me_luts(void) {
    init_me_luts_bd(sad_per_bit16lut_8, QINDEX_RANGE, AOM_BITS_8);
    init_me_luts_bd(sad_per_bit_lut_10, QINDEX_RANGE, AOM_BITS_10);
}
static INLINE int mv_check_bounds(const MvLimits *mv_limits, const MV *mv) {
    return (mv->row >> 3) < mv_limits->row_min || (mv->row >> 3) > mv_limits->row_max ||
           (mv->col >> 3) < mv_limits->col_min || (mv->col >> 3) > mv_limits->col_max;
}
void assert_release(int statement) {
    if (statement == 0) SVT_LOG("ASSERT_ERRRR\n");
}

void intra_bc_search(PictureControlSet *pcs, ModeDecisionContext *context_ptr,
                     const SequenceControlSet *scs, BlkStruct *blk_ptr, MV *dv_cand,
                     uint8_t *num_dv_cand) {
    IntraBcContext  x_st;
    IntraBcContext *x = &x_st;
    uint32_t full_lambda =  context_ptr->hbd_mode_decision ?
        context_ptr->full_lambda_md[EB_10_BIT_MD] :
        context_ptr->full_lambda_md[EB_8_BIT_MD];
    //fill x with what needed.
    x->is_exhaustive_allowed =
        context_ptr->blk_geom->bwidth == 4 || context_ptr->blk_geom->bheight == 4 ? 1 : 0;
    //CHKN crc calculator could be moved to mdContext and these init at init time.
    svt_av1_crc_calculator_init(&x->crc_calculator1, 24, 0x5D6DCB);
    svt_av1_crc_calculator_init(&x->crc_calculator2, 24, 0x864CFB);

    x->xd            = blk_ptr->av1xd;
    x->nmv_vec_cost  = context_ptr->md_rate_estimation_ptr->nmv_vec_cost;
    x->mv_cost_stack = context_ptr->md_rate_estimation_ptr->nmvcoststack;
    BlockSize bsize  = context_ptr->blk_geom->bsize;
    assert(bsize < BlockSizeS_ALL);
    FrameHeader *          frm_hdr    = &pcs->parent_pcs_ptr->frm_hdr;
    const Av1Common *const cm         = pcs->parent_pcs_ptr->av1_cm;
    MvReferenceFrame       ref_frame  = INTRA_FRAME;
    const int              num_planes = 3;
    MacroBlockD *          xd         = blk_ptr->av1xd;
    const TileInfo *       tile       = &xd->tile;
    const int              mi_row     = -xd->mb_to_top_edge / (8 * MI_SIZE);
    const int              mi_col     = -xd->mb_to_left_edge / (8 * MI_SIZE);
    const int              w          = block_size_wide[bsize];
    const int              h          = block_size_high[bsize];
    const int              sb_row     = mi_row >> scs->seq_header.sb_size_log2;
    const int              sb_col     = mi_col >> scs->seq_header.sb_size_log2;

    // Set up limit values for MV components.
    // Mv beyond the range do not produce new/different prediction block.
    const int mi_width   = mi_size_wide[bsize];
    const int mi_height  = mi_size_high[bsize];
    x->mv_limits.row_min = -(((mi_row + mi_height) * MI_SIZE) + AOM_INTERP_EXTEND);
    x->mv_limits.col_min = -(((mi_col + mi_width) * MI_SIZE) + AOM_INTERP_EXTEND);
    x->mv_limits.row_max = (cm->mi_rows - mi_row) * MI_SIZE + AOM_INTERP_EXTEND;
    x->mv_limits.col_max = (cm->mi_cols - mi_col) * MI_SIZE + AOM_INTERP_EXTEND;
    //set search paramters
    x->sadperbit16 = sad_per_bit16lut_8[frm_hdr->quantization_params.base_q_idx];
    x->errorperbit = full_lambda >> RD_EPB_SHIFT;
    x->errorperbit += (x->errorperbit == 0);
    //temp buffer for hash me
    for (int xi = 0; xi < 2; xi++)
        for (int yj = 0; yj < 2; yj++)
            x->hash_value_buffer[xi][yj] =
                (uint32_t *)malloc(AOM_BUFFER_SIZE_FOR_BLOCK_HASH * sizeof(uint32_t));

    IntMv nearestmv, nearmv;
    svt_av1_find_best_ref_mvs_from_stack(
        0,
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
            .ed_ref_mv_stack /*mbmi_ext*/,
        xd,
        ref_frame,
        &nearestmv,
        &nearmv,
        0);
    if (nearestmv.as_int == INVALID_MV) nearestmv.as_int = 0;
    if (nearmv.as_int == INVALID_MV) nearmv.as_int = 0;
    IntMv dv_ref = nearestmv.as_int == 0 ? nearmv : nearestmv;
    if (dv_ref.as_int == 0)
        av1_find_ref_dv(&dv_ref, tile, scs->seq_header.sb_mi_size, mi_row, mi_col);
    // Ref DV should not have sub-pel.
    assert((dv_ref.as_mv.col & 7) == 0);
    assert((dv_ref.as_mv.row & 7) == 0);
    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
        .ed_ref_mv_stack[INTRA_FRAME][0]
        .this_mv = dv_ref;

    /* pointer to current frame */
    Yv12BufferConfig cur_buf;
    link_eb_to_aom_buffer_desc_8bit(pcs->parent_pcs_ptr->enhanced_picture_ptr, &cur_buf);
    struct Buf2D yv12_mb[MAX_MB_PLANE];
    svt_av1_setup_pred_block(bsize, yv12_mb, &cur_buf, mi_row, mi_col);
    for (int i = 0; i < num_planes; ++i) x->xdplane[i].pre[0] = yv12_mb[i]; //ref in ME
    //setup src for DV search same as ref
    x->plane[0].src = x->xdplane[0].pre[0];

    enum IntrabcMotionDirection { IBC_MOTION_ABOVE, IBC_MOTION_LEFT, IBC_MOTION_DIRECTIONS };

    //up to two dv candidates will be generated
    //IBC Modes:   0: OFF 1:Slow   2:Faster   3:Fastest
    enum IntrabcMotionDirection max_dir =
        pcs->parent_pcs_ptr->ibc_mode > 2 ? IBC_MOTION_LEFT : IBC_MOTION_DIRECTIONS;

    for (enum IntrabcMotionDirection dir = IBC_MOTION_ABOVE; dir < max_dir; ++dir) {
        const MvLimits tmp_mv_limits = x->mv_limits;

        switch (dir) {
        case IBC_MOTION_ABOVE:
            x->mv_limits.col_min = (tile->mi_col_start - mi_col) * MI_SIZE;
            x->mv_limits.col_max = (tile->mi_col_end - mi_col) * MI_SIZE - w;
            x->mv_limits.row_min = (tile->mi_row_start - mi_row) * MI_SIZE;
            x->mv_limits.row_max = (sb_row * scs->seq_header.sb_mi_size - mi_row) * MI_SIZE - h;
            break;
        case IBC_MOTION_LEFT:
            x->mv_limits.col_min = (tile->mi_col_start - mi_col) * MI_SIZE;
            x->mv_limits.col_max = (sb_col * scs->seq_header.sb_mi_size - mi_col) * MI_SIZE - w;
            // TODO: Minimize the overlap between above and
            // left areas.
            x->mv_limits.row_min = (tile->mi_row_start - mi_row) * MI_SIZE;
            int bottom_coded_mi_edge =
                AOMMIN((sb_row + 1) * scs->seq_header.sb_mi_size, tile->mi_row_end);
            x->mv_limits.row_max = (bottom_coded_mi_edge - mi_row) * MI_SIZE - h;
            break;
        default: assert(0);
        }
        assert_release(x->mv_limits.col_min >= tmp_mv_limits.col_min);
        assert_release(x->mv_limits.col_max <= tmp_mv_limits.col_max);
        assert_release(x->mv_limits.row_min >= tmp_mv_limits.row_min);
        assert_release(x->mv_limits.row_max <= tmp_mv_limits.row_max);

        svt_av1_set_mv_search_range(&x->mv_limits, &dv_ref.as_mv);

        if (x->mv_limits.col_max < x->mv_limits.col_min ||
            x->mv_limits.row_max < x->mv_limits.row_min) {
            x->mv_limits = tmp_mv_limits;
            continue;
        }

        int step_param = 0;
        MV  mvp_full   = dv_ref.as_mv;
        mvp_full.col >>= 3;
        mvp_full.row >>= 3;
        const int sadpb   = x->sadperbit16;
        x->best_mv.as_int = 0;

#define INT_VAR_MAX 2147483647 // maximum (signed) int value

        const int bestsme = svt_av1_full_pixel_search(pcs,
                                                      x,
                                                      bsize,
                                                      &mvp_full,
                                                      step_param,
                                                      1,
                                                      0,
                                                      sadpb,
                                                      NULL,
                                                      &dv_ref.as_mv,
                                                      INT_VAR_MAX,
                                                      1,
                                                      (MI_SIZE * mi_col),
                                                      (MI_SIZE * mi_row),
                                                      1);

        x->mv_limits = tmp_mv_limits;
        if (bestsme == INT_VAR_MAX) continue;
        mvp_full = x->best_mv.as_mv;

        const MV dv = {.row = mvp_full.row * 8, .col = mvp_full.col * 8};
        if (mv_check_bounds(&x->mv_limits, &dv)) continue;
        if (!av1_is_dv_valid(dv, xd, mi_row, mi_col, bsize, scs->seq_header.sb_size_log2)) continue;

        // DV should not have sub-pel.
        assert_release((dv.col & 7) == 0);
        assert_release((dv.row & 7) == 0);

        //store output
        dv_cand[*num_dv_cand] = dv;
        (*num_dv_cand)++;
    }

    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) free(x->hash_value_buffer[i][j]);
}
void svt_init_mv_cost_params(MV_COST_PARAMS *mv_cost_params,
    ModeDecisionContext *context_ptr,
    const MV *ref_mv, uint8_t base_q_idx, uint32_t rdmult, uint8_t hbd_mode_decision) {
    mv_cost_params->ref_mv = ref_mv;
    mv_cost_params->full_ref_mv = get_fullmv_from_mv(ref_mv);
    mv_cost_params->mv_cost_type = MV_COST_ENTROPY;
    mv_cost_params->error_per_bit = AOMMAX(rdmult >> RD_EPB_SHIFT, 1);
    mv_cost_params->sad_per_bit = hbd_mode_decision ? sad_per_bit_lut_10[base_q_idx] : sad_per_bit16lut_8[base_q_idx];
    mv_cost_params->mvjcost = context_ptr->md_rate_estimation_ptr->nmv_vec_cost;
    mv_cost_params->mvcost[0] = context_ptr->md_rate_estimation_ptr->nmvcoststack[0];
    mv_cost_params->mvcost[1] = context_ptr->md_rate_estimation_ptr->nmvcoststack[1];
}
void inject_intra_bc_candidates(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                                const SequenceControlSet *scs_ptr, BlkStruct *blk_ptr,
                                uint32_t *cand_cnt) {
    MV      dv_cand[2];
    uint8_t num_dv_cand = 0;

    //perform dv-pred + search up to 2 dv(s)
    intra_bc_search(pcs_ptr, context_ptr, scs_ptr, blk_ptr, dv_cand, &num_dv_cand);

    ModeDecisionCandidate *cand_array = context_ptr->fast_candidate_array;
    uint32_t               dv_i;

    for (dv_i = 0; dv_i < num_dv_cand; dv_i++) {
        cand_array[*cand_cnt].palette_info = NULL;
        cand_array[*cand_cnt].type                             = INTRA_MODE;
        cand_array[*cand_cnt].intra_luma_mode                  = DC_PRED;
#if !CLN_MD_CANDS
        cand_array[*cand_cnt].distortion_ready                 = 0;
#endif
        cand_array[*cand_cnt].use_intrabc                      = 1;
        cand_array[*cand_cnt].is_directional_mode_flag         = 0;
        cand_array[*cand_cnt].angle_delta[PLANE_TYPE_Y]        = 0;
        cand_array[*cand_cnt].intra_chroma_mode                = UV_DC_PRED;
        cand_array[*cand_cnt].cfl_alpha_signs                  = 0;
        cand_array[*cand_cnt].cfl_alpha_idx                    = 0;
        cand_array[*cand_cnt].is_directional_chroma_mode_flag  = 0;
        cand_array[*cand_cnt].angle_delta[PLANE_TYPE_UV]       = 0;
        cand_array[*cand_cnt].transform_type[0]                = DCT_DCT;
        cand_array[*cand_cnt].transform_type_uv                = DCT_DCT;
        cand_array[*cand_cnt].ref_frame_type                   = INTRA_FRAME;
        cand_array[*cand_cnt].pred_mode                        = DC_PRED;
        cand_array[*cand_cnt].motion_mode                      = SIMPLE_TRANSLATION;
        //inter ralated
        cand_array[*cand_cnt].is_compound             = 0;
        cand_array[*cand_cnt].is_interintra_used      = 0;
        cand_array[*cand_cnt].skip_mode_allowed              = EB_FALSE;
        cand_array[*cand_cnt].prediction_direction[0] = UNI_PRED_LIST_0;
#if !CLN_MD_CANDS
        cand_array[*cand_cnt].is_new_mv               = 0;
#endif
        cand_array[*cand_cnt].motion_vector_xl0       = dv_cand[dv_i].col;
        cand_array[*cand_cnt].motion_vector_yl0       = dv_cand[dv_i].row;
        cand_array[*cand_cnt].motion_vector_pred_x[REF_LIST_0] =
            context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                .ed_ref_mv_stack[INTRA_FRAME][0]
                .this_mv.as_mv.col;
        cand_array[*cand_cnt].motion_vector_pred_y[REF_LIST_0] =
            context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                .ed_ref_mv_stack[INTRA_FRAME][0]
                .this_mv.as_mv.row;
        cand_array[*cand_cnt].drl_index         = 0;
#if !CLN_MD_CANDS
        cand_array[*cand_cnt].ref_mv_index      = 0;
#endif
        cand_array[*cand_cnt].interp_filters    = av1_broadcast_interp_filter(BILINEAR);
        cand_array[*cand_cnt].filter_intra_mode = FILTER_INTRA_MODES;
        INCRMENT_CAND_TOTAL_COUNT((*cand_cnt));
    }
}
// Indices are sign, integer, and fractional part of the gradient value
static const uint8_t gradient_to_angle_bin[2][7][16] = {
    {
        {6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
        {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
        {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
    },
    {
        {6, 6, 6, 6, 5, 5, 5, 5, 5, 5, 5, 5, 4, 4, 4, 4},
        {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3},
        {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3},
        {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3},
        {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3},
        {3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2},
        {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
    },
};

/* clang-format off */
static const uint8_t mode_to_angle_bin[INTRA_MODES] = {
  0, 2, 6, 0, 4, 3, 5, 7, 1, 0,
  0,
};
void svt_av1_get_gradient_hist_c(const uint8_t *src, int src_stride, int rows,
    int cols, uint64_t *hist) {
    src += src_stride;
    for (int r = 1; r < rows; ++r) {
        for (int c = 1; c < cols; ++c) {
            int dx = src[c] - src[c - 1];
            int dy = src[c] - src[c - src_stride];
            int index;
            const int temp = dx * dx + dy * dy;
            if (dy == 0) {
                index = 2;
            }
            else {
                const int sn = (dx > 0) ^ (dy > 0);
                dx = abs(dx);
                dy = abs(dy);
                const int remd = (dx % dy) * 16 / dy;
                const int quot = dx / dy;
                index = gradient_to_angle_bin[sn][AOMMIN(quot, 6)][AOMMIN(remd, 15)];
            }
            hist[index] += temp;
        }
        src += src_stride;
    }
}
 void angle_estimation(
    const uint8_t *src,
    int src_stride,
    int rows,
    int cols,
    //BLOCK_SIZE bsize,
    uint8_t *directional_mode_skip_mask)
{
    // Check if angle_delta is used
    //if (!av1_use_angle_delta(bsize, need access to context)) return;

    uint64_t hist[DIRECTIONAL_MODES] = { 0 };
    //if (is_hbd)
    //    get_highbd_gradient_hist(src, src_stride, rows, cols, hist);
    //else
    svt_av1_get_gradient_hist(src, src_stride, rows, cols, hist);

    int i;
    uint64_t hist_sum = 0;
    for (i = 0; i < DIRECTIONAL_MODES; ++i) hist_sum += hist[i];
    for (i = 0; i < INTRA_MODES; ++i) {
        if (av1_is_directional_mode(i)) {
            const uint8_t angle_bin = mode_to_angle_bin[i];
            uint64_t score = 2 * hist[angle_bin];
            int weight = 2;
            if (angle_bin > 0) {
                score += hist[angle_bin - 1];
                ++weight;
            }
            if (angle_bin < DIRECTIONAL_MODES - 1) {
                score += hist[angle_bin + 1];
                ++weight;
            }
            const int thresh = 10;
            if (score * thresh < hist_sum * weight) directional_mode_skip_mask[i] = 1;
        }
    }
}
// END of Function Declarations
void  inject_intra_candidates(
    PictureControlSet            *pcs_ptr,
    ModeDecisionContext          *context_ptr,
    const SequenceControlSet     *scs_ptr,
    SuperBlock                   *sb_ptr,
    EbBool                        dc_cand_only_flag,
    uint32_t                     *candidate_total_cnt){
    if (context_ptr->skip_intra)
        return;
    (void)scs_ptr;
    (void)sb_ptr;
    FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;
    uint8_t                     intra_mode_start = DC_PRED;
    uint8_t                     intra_mode_end = dc_cand_only_flag ? DC_PRED :
                                                 context_ptr->md_enable_paeth ? PAETH_PRED :
                                                 context_ptr->md_enable_smooth ? SMOOTH_H_PRED : D67_PRED;
    uint8_t                     open_loop_intra_candidate;
    uint32_t                    cand_total_cnt = 0;
    EbBool                      use_angle_delta = av1_use_angle_delta(context_ptr->blk_geom->bsize, context_ptr->md_intra_angle_delta);
    uint8_t                     angle_delta_candidate_count = use_angle_delta ? 7 : 1;
    ModeDecisionCandidate    *cand_array = context_ptr->fast_candidate_array;
    EbBool                      disable_cfl_flag = (MAX(context_ptr->blk_geom->bheight, context_ptr->blk_geom->bwidth) > 32) ? EB_TRUE : EB_FALSE;
    disable_cfl_flag = context_ptr->md_disable_cfl ? EB_TRUE : disable_cfl_flag;
    uint8_t                     disable_z2_prediction;
    uint8_t                     disable_angle_prediction;
    uint8_t directional_mode_skip_mask[INTRA_MODES] = { 0 };

    if (scs_ptr->static_config.disable_cfl_flag != DEFAULT && !disable_cfl_flag)
        // if disable_cfl_flag == 1 then it doesn't matter what cli says otherwise change it to cli
        disable_cfl_flag = (EbBool)scs_ptr->static_config.disable_cfl_flag;

    uint8_t     angle_delta_shift = 1;
    if (context_ptr->disable_angle_z2_intra_flag) {
        disable_angle_prediction = 1;
        angle_delta_candidate_count = 1;
        angle_delta_shift = 1;
        disable_z2_prediction = 1;
    } else if (pcs_ptr->parent_pcs_ptr->intra_pred_mode == 4) {
        if (pcs_ptr->slice_type == I_SLICE) {
            intra_mode_end = context_ptr->md_enable_paeth ? PAETH_PRED :
                             context_ptr->md_enable_smooth ? SMOOTH_H_PRED : D67_PRED;
            angle_delta_candidate_count = use_angle_delta ? 5 : 1;
            disable_angle_prediction = 0;
            angle_delta_shift = 2;
            disable_z2_prediction = 0;
        }
        else {
            intra_mode_end = DC_PRED;
            disable_angle_prediction = 1;
            angle_delta_candidate_count = 1;
            angle_delta_shift = 1;
            disable_z2_prediction = 0;
        }
    } else if (pcs_ptr->parent_pcs_ptr->intra_pred_mode == 3){
        disable_z2_prediction       = 0;
        disable_angle_prediction    = 1;
    } else if (pcs_ptr->parent_pcs_ptr->intra_pred_mode == 2) {
        disable_z2_prediction       = 0;
        disable_angle_prediction    = (context_ptr->blk_geom->sq_size > 16 ||
                                       context_ptr->blk_geom->bwidth == 4 ||
                                       context_ptr->blk_geom->bheight == 4) ? 1 : 0;
    } else if (pcs_ptr->parent_pcs_ptr->intra_pred_mode == 1) {
        disable_z2_prediction       = (context_ptr->blk_geom->sq_size > 16 ||
                                       context_ptr->blk_geom->bwidth == 4 ||
                                       context_ptr->blk_geom->bheight == 4) ? 1 : 0;
        disable_angle_prediction    = 0;
        if (context_ptr->blk_geom->sq_size > 16 ||
            context_ptr->blk_geom->bwidth == 4 ||
            context_ptr->blk_geom->bheight == 4)
            angle_delta_candidate_count = 1;
    } else {
        disable_z2_prediction       = 0;
        disable_angle_prediction    = 0;
    }
    for (open_loop_intra_candidate = intra_mode_start; open_loop_intra_candidate <= intra_mode_end ; ++open_loop_intra_candidate) {
        if (av1_is_directional_mode((PredictionMode)open_loop_intra_candidate)) {
            if (!disable_angle_prediction &&
                directional_mode_skip_mask[(PredictionMode)open_loop_intra_candidate] == 0) {
                for (uint8_t angle_delta_counter = 0; angle_delta_counter < angle_delta_candidate_count; ++angle_delta_counter) {
                    int32_t angle_delta = CLIP( angle_delta_shift * (angle_delta_candidate_count == 1 ? 0 : angle_delta_counter - (angle_delta_candidate_count >> 1)), -3 , 3);
                    int32_t  p_angle = mode_to_angle_map[(PredictionMode)open_loop_intra_candidate] + angle_delta * ANGLE_STEP;
                    if (!disable_z2_prediction || (p_angle <= 90 || p_angle >= 180)) {
                        cand_array[cand_total_cnt].type = INTRA_MODE;
                        cand_array[cand_total_cnt].skip_mode_allowed = EB_FALSE;
                        cand_array[cand_total_cnt].palette_info = NULL;
                        cand_array[cand_total_cnt].intra_luma_mode = open_loop_intra_candidate;
#if !CLN_MD_CANDS
                        cand_array[cand_total_cnt].distortion_ready = 0;
#endif
                        cand_array[cand_total_cnt].use_intrabc = 0;
                        cand_array[cand_total_cnt].filter_intra_mode = FILTER_INTRA_MODES;
                        cand_array[cand_total_cnt].is_directional_mode_flag = (uint8_t)av1_is_directional_mode((PredictionMode)open_loop_intra_candidate);
                        cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_Y] = angle_delta;
                        // Search the best independent intra chroma mode
                        if (context_ptr->chroma_level == CHROMA_MODE_0) {
                            cand_array[cand_total_cnt].intra_chroma_mode = disable_cfl_flag ?
                                context_ptr->best_uv_mode[open_loop_intra_candidate][MAX_ANGLE_DELTA + cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_Y]] :
                                UV_CFL_PRED ;
                            cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_UV] = disable_cfl_flag ?
                                context_ptr->best_uv_angle[cand_array[cand_total_cnt].intra_luma_mode][MAX_ANGLE_DELTA + cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_Y]] : 0;
                            cand_array[cand_total_cnt].is_directional_chroma_mode_flag = disable_cfl_flag ?
                                (uint8_t)av1_is_directional_mode((PredictionMode)(context_ptr->best_uv_mode[cand_array[cand_total_cnt].intra_luma_mode][MAX_ANGLE_DELTA + cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_Y]])) : 0;
                        }
                        else {
                            // Hsan/Omar: why the restriction below ? (i.e. disable_ang_uv)
                            const int32_t disable_ang_uv = (context_ptr->blk_geom->bwidth == 4 || context_ptr->blk_geom->bheight == 4) && context_ptr->blk_geom->has_uv ? 1 : 0;
                            cand_array[cand_total_cnt].intra_chroma_mode = disable_cfl_flag ?
                                intra_luma_to_chroma[open_loop_intra_candidate] :
                                (context_ptr->chroma_level == CHROMA_MODE_1) ?
                                UV_CFL_PRED :
                                UV_DC_PRED;
                            cand_array[cand_total_cnt].intra_chroma_mode = disable_ang_uv && av1_is_directional_mode(cand_array[cand_total_cnt].intra_chroma_mode) ?
                                UV_DC_PRED : cand_array[cand_total_cnt].intra_chroma_mode;
                            cand_array[cand_total_cnt].is_directional_chroma_mode_flag = (uint8_t)av1_is_directional_mode((PredictionMode)cand_array[cand_total_cnt].intra_chroma_mode);
                            cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_UV] = 0;
                        }
                        cand_array[cand_total_cnt].cfl_alpha_signs = 0;
                        cand_array[cand_total_cnt].cfl_alpha_idx = 0;
                        cand_array[cand_total_cnt].transform_type[0] = DCT_DCT;

                        if (cand_array[cand_total_cnt].intra_chroma_mode == UV_CFL_PRED)
                            cand_array[cand_total_cnt].transform_type_uv = DCT_DCT;
                        else
                            cand_array[cand_total_cnt].transform_type_uv =
#if RFCTR_INTRA_TX_INIT_FUNC
                            av1_get_tx_type(
                                0, // is_inter
                                (PredictionMode)cand_array[cand_total_cnt].intra_luma_mode,
                                (UvPredictionMode)cand_array[cand_total_cnt].intra_chroma_mode,
                                PLANE_TYPE_UV,
                                context_ptr->blk_geom->txsize_uv[0][0],
                                frm_hdr->reduced_tx_set);
#else
                            av1_get_tx_type(
                                context_ptr->blk_geom->bsize,
                                0,
                                (PredictionMode)cand_array[cand_total_cnt].intra_luma_mode,
                                (UvPredictionMode)cand_array[cand_total_cnt].intra_chroma_mode,
                                PLANE_TYPE_UV,
                                0,
                                0,
                                0,
                                context_ptr->blk_geom->txsize_uv[0][0],
                                frm_hdr->reduced_tx_set);
#endif
                        cand_array[cand_total_cnt].ref_frame_type = INTRA_FRAME;
                        cand_array[cand_total_cnt].pred_mode = (PredictionMode)open_loop_intra_candidate;
                        cand_array[cand_total_cnt].motion_mode = SIMPLE_TRANSLATION;
                        cand_array[cand_total_cnt].is_interintra_used = 0;
                        INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
                    }
            }
        }
        }
        else {
            cand_array[cand_total_cnt].type = INTRA_MODE;
            cand_array[cand_total_cnt].skip_mode_allowed = EB_FALSE;
            cand_array[cand_total_cnt].palette_info = NULL;
            cand_array[cand_total_cnt].intra_luma_mode = open_loop_intra_candidate;
#if !CLN_MD_CANDS
            cand_array[cand_total_cnt].distortion_ready = 0;
#endif
            cand_array[cand_total_cnt].use_intrabc = 0;
            cand_array[cand_total_cnt].filter_intra_mode = FILTER_INTRA_MODES;
            cand_array[cand_total_cnt].is_directional_mode_flag = (uint8_t)av1_is_directional_mode((PredictionMode)open_loop_intra_candidate);
            cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_Y] = 0;
            // Search the best independent intra chroma mode
            if (context_ptr->chroma_level == CHROMA_MODE_0) {
                cand_array[cand_total_cnt].intra_chroma_mode = disable_cfl_flag ?
                    context_ptr->best_uv_mode[open_loop_intra_candidate][MAX_ANGLE_DELTA + cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_Y]] :
                    UV_CFL_PRED;
                cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_UV] = disable_cfl_flag ?
                    context_ptr->best_uv_angle[cand_array[cand_total_cnt].intra_luma_mode][MAX_ANGLE_DELTA + cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_Y]] : 0;
                cand_array[cand_total_cnt].is_directional_chroma_mode_flag = disable_cfl_flag ?
                    (uint8_t)av1_is_directional_mode((PredictionMode)(context_ptr->best_uv_mode[cand_array[cand_total_cnt].intra_luma_mode][MAX_ANGLE_DELTA + cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_Y]])) : 0;
            }
            else {
                // Hsan/Omar: why the restriction below ? (i.e. disable_ang_uv)
                const int32_t disable_ang_uv = (context_ptr->blk_geom->bwidth == 4 || context_ptr->blk_geom->bheight == 4) && context_ptr->blk_geom->has_uv ? 1 : 0;
                cand_array[cand_total_cnt].intra_chroma_mode = disable_cfl_flag ?
                    intra_luma_to_chroma[open_loop_intra_candidate] :
                    (context_ptr->chroma_level == CHROMA_MODE_1) ?
                        UV_CFL_PRED :
                        UV_DC_PRED;

                cand_array[cand_total_cnt].intra_chroma_mode = disable_ang_uv && av1_is_directional_mode(cand_array[cand_total_cnt].intra_chroma_mode) ?
                    UV_DC_PRED : cand_array[cand_total_cnt].intra_chroma_mode;

                cand_array[cand_total_cnt].is_directional_chroma_mode_flag = (uint8_t)av1_is_directional_mode((PredictionMode)cand_array[cand_total_cnt].intra_chroma_mode);
                cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_UV] = 0;

            }
            cand_array[cand_total_cnt].cfl_alpha_signs = 0;
            cand_array[cand_total_cnt].cfl_alpha_idx = 0;
            cand_array[cand_total_cnt].transform_type[0] = DCT_DCT;

            if (cand_array[cand_total_cnt].intra_chroma_mode == UV_CFL_PRED)
                cand_array[cand_total_cnt].transform_type_uv = DCT_DCT;
            else
                cand_array[cand_total_cnt].transform_type_uv =
#if RFCTR_INTRA_TX_INIT_FUNC
                av1_get_tx_type(
                    0, // is_inter
                    (PredictionMode)cand_array[cand_total_cnt].intra_luma_mode,
                    (UvPredictionMode)cand_array[cand_total_cnt].intra_chroma_mode,
                    PLANE_TYPE_UV,
                    context_ptr->blk_geom->txsize_uv[0][0],
                    frm_hdr->reduced_tx_set);
#else
                av1_get_tx_type(
                    context_ptr->blk_geom->bsize,
                    0,
                    (PredictionMode)cand_array[cand_total_cnt].intra_luma_mode,
                    (UvPredictionMode)cand_array[cand_total_cnt].intra_chroma_mode,
                    PLANE_TYPE_UV,
                    0,
                    0,
                    0,
                    context_ptr->blk_geom->txsize_uv[0][0],
                    frm_hdr->reduced_tx_set);
#endif
            cand_array[cand_total_cnt].ref_frame_type = INTRA_FRAME;
            cand_array[cand_total_cnt].pred_mode = (PredictionMode)open_loop_intra_candidate;
            cand_array[cand_total_cnt].motion_mode = SIMPLE_TRANSLATION;
            cand_array[cand_total_cnt].is_interintra_used = 0;
            INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
        }
    }

    // update the total number of candidates injected
    (*candidate_total_cnt) = cand_total_cnt;

    return;
}
// END of Function Declarations
void  inject_filter_intra_candidates(
    PictureControlSet            *pcs_ptr,
    ModeDecisionContext          *context_ptr,
    uint32_t                     *candidate_total_cnt){
    if (context_ptr->skip_intra)
        return;
    FilterIntraMode             intra_mode_start = FILTER_DC_PRED;
    FilterIntraMode             intra_mode_end   = FILTER_INTRA_MODES;

    FilterIntraMode             filter_intra_mode;
    uint32_t                    cand_total_cnt = *candidate_total_cnt;
    ModeDecisionCandidate      *cand_array = context_ptr->fast_candidate_array;

    EbBool                      disable_cfl_flag = (MAX(context_ptr->blk_geom->bheight, context_ptr->blk_geom->bwidth) > 32) ? EB_TRUE : EB_FALSE;
    disable_cfl_flag = context_ptr->md_disable_cfl ? EB_TRUE : disable_cfl_flag;
    SequenceControlSet *scs_ptr = (SequenceControlSet*)pcs_ptr->scs_wrapper_ptr->object_ptr;
    if (scs_ptr->static_config.disable_cfl_flag != DEFAULT && !disable_cfl_flag)
        // if disable_cfl_flag == 1 then it doesn't matter what cli says otherwise change it to cli
        disable_cfl_flag = (EbBool)scs_ptr->static_config.disable_cfl_flag;

    FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;

    for (filter_intra_mode = intra_mode_start; filter_intra_mode < intra_mode_end ; ++filter_intra_mode) {

            if (filter_intra_mode == FILTER_PAETH_PRED && !context_ptr->md_enable_paeth)
                continue;

            cand_array[cand_total_cnt].type = INTRA_MODE;
            cand_array[cand_total_cnt].skip_mode_allowed = EB_FALSE;
            cand_array[cand_total_cnt].intra_luma_mode = DC_PRED;
#if !CLN_MD_CANDS
            cand_array[cand_total_cnt].distortion_ready = 0;
#endif
            cand_array[cand_total_cnt].use_intrabc = 0;
            cand_array[cand_total_cnt].filter_intra_mode = filter_intra_mode;
            cand_array[cand_total_cnt].is_directional_mode_flag = 0;
            cand_array[cand_total_cnt].palette_info = NULL;
            cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_Y] = 0;

            // Search the best independent intra chroma mode
            if (context_ptr->chroma_level == CHROMA_MODE_0) {
                cand_array[cand_total_cnt].intra_chroma_mode  = disable_cfl_flag ? context_ptr->best_uv_mode[fimode_to_intramode[filter_intra_mode]][MAX_ANGLE_DELTA + cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_Y]] : UV_CFL_PRED;

                cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_UV] = disable_cfl_flag ? context_ptr->best_uv_angle[fimode_to_intramode[filter_intra_mode]][MAX_ANGLE_DELTA + cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_Y]] : 0;
                cand_array[cand_total_cnt].is_directional_chroma_mode_flag = disable_cfl_flag ? (uint8_t)av1_is_directional_mode((PredictionMode)(context_ptr->best_uv_mode[fimode_to_intramode[filter_intra_mode]][MAX_ANGLE_DELTA + cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_Y]])) : 0;

            }
            else {
                // Hsan/Omar: why the restriction below ? (i.e. disable_ang_uv)
                const int32_t disable_ang_uv = (context_ptr->blk_geom->bwidth == 4 || context_ptr->blk_geom->bheight == 4) && context_ptr->blk_geom->has_uv ? 1 : 0;
                cand_array[cand_total_cnt].intra_chroma_mode = disable_cfl_flag ? intra_luma_to_chroma[fimode_to_intramode[filter_intra_mode]] :
                    (context_ptr->chroma_level == CHROMA_MODE_1) ?
                        UV_CFL_PRED :
                        UV_DC_PRED;

                cand_array[cand_total_cnt].intra_chroma_mode =  disable_ang_uv && av1_is_directional_mode(cand_array[cand_total_cnt].intra_chroma_mode) ?
                    UV_DC_PRED : cand_array[cand_total_cnt].intra_chroma_mode;

                cand_array[cand_total_cnt].is_directional_chroma_mode_flag = (uint8_t)av1_is_directional_mode((PredictionMode)cand_array[cand_total_cnt].intra_chroma_mode);
                cand_array[cand_total_cnt].angle_delta[PLANE_TYPE_UV] = 0;
            }

            cand_array[cand_total_cnt].cfl_alpha_signs = 0;
            cand_array[cand_total_cnt].cfl_alpha_idx = 0;
            cand_array[cand_total_cnt].transform_type[0] = DCT_DCT;

            if (cand_array[cand_total_cnt].intra_chroma_mode == UV_CFL_PRED)
                cand_array[cand_total_cnt].transform_type_uv = DCT_DCT;
            else
                cand_array[cand_total_cnt].transform_type_uv =
#if RFCTR_INTRA_TX_INIT_FUNC
                av1_get_tx_type(
                    0, // is_inter
                    (PredictionMode)cand_array[cand_total_cnt].intra_luma_mode,
                    (UvPredictionMode)cand_array[cand_total_cnt].intra_chroma_mode,
                    PLANE_TYPE_UV,
                    context_ptr->blk_geom->txsize_uv[0][0],
                    frm_hdr->reduced_tx_set);
#else
                av1_get_tx_type(
                    context_ptr->blk_geom->bsize,
                    0,
                    (PredictionMode)cand_array[cand_total_cnt].intra_luma_mode,
                    (UvPredictionMode)cand_array[cand_total_cnt].intra_chroma_mode,
                    PLANE_TYPE_UV,
                    0,
                    0,
                    0,
                    context_ptr->blk_geom->txsize_uv[0][0],
                    frm_hdr->reduced_tx_set);
#endif
            cand_array[cand_total_cnt].ref_frame_type = INTRA_FRAME;
            cand_array[cand_total_cnt].pred_mode = DC_PRED;
            cand_array[cand_total_cnt].motion_mode = SIMPLE_TRANSLATION;
            cand_array[cand_total_cnt].is_interintra_used = 0;
            INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);
    }

    // update the total number of candidates injected
    (*candidate_total_cnt) = cand_total_cnt;

    return;
}
void inject_zz_backup_candidate(
    struct ModeDecisionContext *context_ptr,
    uint32_t *candidate_total_cnt) {
    ModeDecisionCandidate *cand_array = context_ptr->fast_candidate_array;
    IntMv                  best_pred_mv[2] = { {0}, {0} };
    uint32_t               cand_total_cnt = (*candidate_total_cnt);

    cand_array[cand_total_cnt].type = INTER_MODE;
#if !CLN_MD_CANDS
    cand_array[cand_total_cnt].distortion_ready = 0;
#endif
    cand_array[cand_total_cnt].use_intrabc = 0;
    cand_array[cand_total_cnt].skip_mode_allowed = EB_FALSE;
    cand_array[cand_total_cnt].prediction_direction[0] = (EbPredDirection)0;
#if !CLN_MD_CANDS
    cand_array[cand_total_cnt].inter_mode = NEWMV;
#endif
    cand_array[cand_total_cnt].pred_mode = NEWMV;
    cand_array[cand_total_cnt].motion_mode = SIMPLE_TRANSLATION;
    cand_array[cand_total_cnt].is_compound = 0;
#if !CLN_MD_CANDS
    cand_array[cand_total_cnt].is_new_mv = 1;
#endif
    cand_array[cand_total_cnt].drl_index = 0;

    // zz
    cand_array[cand_total_cnt].motion_vector_xl0 = 0;
    cand_array[cand_total_cnt].motion_vector_yl0 = 0;

    // will be needed later by the rate estimation
#if !CLN_MD_CANDS
    cand_array[cand_total_cnt].ref_mv_index = 0;
#endif
    cand_array[cand_total_cnt].ref_frame_type = svt_get_ref_frame_type(REF_LIST_0, 0);
#if !CLN_MD_CANDS
    cand_array[cand_total_cnt].ref_frame_index_l0 = 0;
    cand_array[cand_total_cnt].ref_frame_index_l1 = -1;
#endif
    cand_array[cand_total_cnt].transform_type[0] = DCT_DCT;
    cand_array[cand_total_cnt].transform_type_uv = DCT_DCT;
    choose_best_av1_mv_pred(context_ptr,
#if CLN_FAST_COST
        context_ptr->md_rate_estimation_ptr,
#else
        cand_array[cand_total_cnt].md_rate_estimation_ptr,
#endif
        context_ptr->blk_ptr,
        cand_array[cand_total_cnt].ref_frame_type,
        cand_array[cand_total_cnt].is_compound,
        cand_array[cand_total_cnt].pred_mode,
        cand_array[cand_total_cnt].motion_vector_xl0,
        cand_array[cand_total_cnt].motion_vector_yl0,
        0,
        0,
        &cand_array[cand_total_cnt].drl_index,
        best_pred_mv);

    cand_array[cand_total_cnt].motion_vector_pred_x[REF_LIST_0] = best_pred_mv[0].as_mv.col;
    cand_array[cand_total_cnt].motion_vector_pred_y[REF_LIST_0] = best_pred_mv[0].as_mv.row;

    cand_array[cand_total_cnt].is_interintra_used = 0;
    cand_array[cand_total_cnt].motion_mode = SIMPLE_TRANSLATION;

    INCRMENT_CAND_TOTAL_COUNT(cand_total_cnt);

    // update the total number of candidates injected
    (*candidate_total_cnt) = cand_total_cnt;
}
int svt_av1_allow_palette(int allow_palette,
    BlockSize sb_type) {
    assert(sb_type < BlockSizeS_ALL);
    return allow_palette && block_size_wide[sb_type] <= 64 &&
        block_size_high[sb_type] <= 64 && sb_type >= BLOCK_8X8;
}
void  search_palette_luma(
    PictureControlSet            *pcs_ptr,
    ModeDecisionContext          *context_ptr,
    PaletteInfo                 *palette_cand,
    uint32_t                     *tot_palette_cands);

void  inject_palette_candidates(
    PictureControlSet            *pcs_ptr,
    ModeDecisionContext          *context_ptr,
    uint32_t                       *candidate_total_cnt) {



    uint32_t                  can_total_cnt = *candidate_total_cnt;
    ModeDecisionCandidate    *cand_array = context_ptr->fast_candidate_array;
    EbBool                    disable_cfl_flag = (MAX(context_ptr->blk_geom->bheight, context_ptr->blk_geom->bwidth) > 32) ? EB_TRUE : EB_FALSE;
    disable_cfl_flag = context_ptr->md_disable_cfl ? EB_TRUE : disable_cfl_flag;
    uint32_t cand_i;
    uint32_t tot_palette_cands = 0;
    PaletteInfo    *palette_cand_array = context_ptr->palette_cand_array;

    SequenceControlSet *scs_ptr = (SequenceControlSet*)pcs_ptr->scs_wrapper_ptr->object_ptr;
    if (scs_ptr->static_config.disable_cfl_flag != DEFAULT && !disable_cfl_flag)
        // if disable_cfl_flag == 1 then it doesn't matter what cli says otherwise change it to cli
        disable_cfl_flag = (EbBool)scs_ptr->static_config.disable_cfl_flag;

    search_palette_luma(
        pcs_ptr,
        context_ptr,
        palette_cand_array,
        &tot_palette_cands);

    for (cand_i = 0; cand_i < tot_palette_cands; ++cand_i) {
        cand_array[can_total_cnt].is_interintra_used = 0;
        palette_cand_array[cand_i].pmi.palette_size[1] = 0;
        cand_array[can_total_cnt].palette_info = &palette_cand_array[cand_i];
        assert(palette_cand_array[cand_i].pmi.palette_size[0] < 9);
        //to re check these fields
        cand_array[can_total_cnt].type = INTRA_MODE;
        cand_array[can_total_cnt].skip_mode_allowed = EB_FALSE;
        cand_array[can_total_cnt].intra_luma_mode = DC_PRED;
#if !CLN_MD_CANDS
        cand_array[can_total_cnt].distortion_ready = 0;
#endif
        cand_array[can_total_cnt].use_intrabc = 0;

        cand_array[can_total_cnt].filter_intra_mode = FILTER_INTRA_MODES;

        cand_array[can_total_cnt].is_directional_mode_flag = 0;

        cand_array[can_total_cnt].angle_delta[PLANE_TYPE_Y] = 0;
        const int32_t disable_ang_uv = (context_ptr->blk_geom->bwidth == 4 || context_ptr->blk_geom->bheight == 4) && context_ptr->blk_geom->has_uv ? 1 : 0;
        cand_array[can_total_cnt].intra_chroma_mode = disable_cfl_flag ?
            intra_luma_to_chroma[DC_PRED] :
            (context_ptr->chroma_level <= CHROMA_MODE_1) ?
            UV_CFL_PRED :
            UV_DC_PRED;

        cand_array[can_total_cnt].intra_chroma_mode = disable_ang_uv && av1_is_directional_mode(cand_array[can_total_cnt].intra_chroma_mode) ?
            UV_DC_PRED : cand_array[can_total_cnt].intra_chroma_mode;

        cand_array[can_total_cnt].is_directional_chroma_mode_flag = (uint8_t)av1_is_directional_mode((PredictionMode)cand_array[can_total_cnt].intra_chroma_mode);
        cand_array[can_total_cnt].angle_delta[PLANE_TYPE_UV] = 0;
        cand_array[can_total_cnt].cfl_alpha_signs = 0;
        cand_array[can_total_cnt].cfl_alpha_idx = 0;
        cand_array[can_total_cnt].transform_type[0] = DCT_DCT;

        if (cand_array[can_total_cnt].intra_chroma_mode == UV_CFL_PRED)
            cand_array[can_total_cnt].transform_type_uv = DCT_DCT;
        else
            cand_array[can_total_cnt].transform_type_uv =
#if RFCTR_INTRA_TX_INIT_FUNC
            av1_get_tx_type(
                0, // is_inter
                (PredictionMode)cand_array[can_total_cnt].intra_luma_mode,
                (UvPredictionMode)cand_array[can_total_cnt].intra_chroma_mode,
                PLANE_TYPE_UV,
                context_ptr->blk_geom->txsize_uv[0][0],
                pcs_ptr->parent_pcs_ptr->frm_hdr.reduced_tx_set);
#else
            av1_get_tx_type(
                context_ptr->blk_geom->bsize,
                0,
                (PredictionMode)cand_array[can_total_cnt].intra_luma_mode,
                (UvPredictionMode)cand_array[can_total_cnt].intra_chroma_mode,
                PLANE_TYPE_UV,
                0,
                0,
                0,
                context_ptr->blk_geom->txsize_uv[0][0],
                pcs_ptr->parent_pcs_ptr->frm_hdr.reduced_tx_set);
#endif
        cand_array[can_total_cnt].ref_frame_type = INTRA_FRAME;
        cand_array[can_total_cnt].pred_mode = (PredictionMode)DC_PRED;
        cand_array[can_total_cnt].motion_mode = SIMPLE_TRANSLATION;
        INCRMENT_CAND_TOTAL_COUNT(can_total_cnt);
    }

    // update the total number of candidates injected
    (*candidate_total_cnt) = can_total_cnt;

    return;
}
#if FTR_PD2_REDUCE_MDS0
static INLINE void eliminate_candidate_based_on_pme_me_results(ModeDecisionContext *context_ptr,
    uint8_t temp_layer_idx,
    uint8_t is_used_as_ref,
    uint8_t *dc_cand_only_flag)
{
    uint32_t th = temp_layer_idx == 0 ? 10 : is_used_as_ref ? 30 : 200;
    if (context_ptr->md_pme_ctrls.enabled || context_ptr->md_subpel_me_ctrls.enabled) {
        th = th * context_ptr->blk_geom->bheight * context_ptr->blk_geom->bwidth;
        const uint32_t best_me_distotion = MIN(MIN(context_ptr->pme_res[0][0].dist, context_ptr->pme_res[1][0].dist), context_ptr->md_me_dist);
#if CLN_CANDIDATE_ELEMINATION_CTR
        if (best_me_distotion < th) {
            *dc_cand_only_flag = context_ptr->cand_elimination_ctrs.dc_only ? 1 : *dc_cand_only_flag;
            context_ptr->inject_new_warp = context_ptr->cand_elimination_ctrs.inject_new_warp ? 0 : context_ptr->inject_new_warp;
        }
        else
            context_ptr->inject_new_warp = context_ptr->cand_elimination_ctrs.inject_new_warp ? 2 : context_ptr->inject_new_warp;
#if TUNE_M9_PME
        if (context_ptr->md_pme_ctrls.enabled) {
#endif
        const int32_t me_pme_distance = (context_ptr->md_me_dist - MIN(context_ptr->pme_res[0][0].dist, context_ptr->pme_res[1][0].dist));
        if (me_pme_distance >= 0)
            context_ptr->inject_new_me = context_ptr->cand_elimination_ctrs.inject_new_me ? 0 : context_ptr->inject_new_me;
        else
            context_ptr->inject_new_pme = context_ptr->cand_elimination_ctrs.inject_new_pme ? 0 : context_ptr->inject_new_pme;
#if TUNE_M9_PME
        }
#endif
#else

        if (best_me_distotion < th) {
            *dc_cand_only_flag = 1;
            context_ptr->inject_new_warp = 0;
        }
        else
            context_ptr->inject_new_warp = 2;
#if TUNE_M9_PME
        if (context_ptr->md_pme_ctrls.enabled) {
#endif
        const int32_t me_pme_distance = (context_ptr->md_me_dist - MIN(context_ptr->pme_res[0][0].dist, context_ptr->pme_res[1][0].dist));
        if (me_pme_distance >= 0)
            context_ptr->inject_new_me = 0;
        else
            context_ptr->inject_new_pme = 0;
#if TUNE_M9_PME
        }
#endif
#endif
    }
}
#endif
EbErrorType generate_md_stage_0_cand(
    SuperBlock          *sb_ptr,
    ModeDecisionContext *context_ptr,
    uint32_t            *candidate_total_count_ptr,
    PictureControlSet   *pcs_ptr)
{

    const SequenceControlSet *scs_ptr = (SequenceControlSet*)pcs_ptr->scs_wrapper_ptr->object_ptr;
    const EB_SLICE slice_type = pcs_ptr->slice_type;
    uint32_t cand_total_cnt = 0;
    // Reset duplicates variables
    context_ptr->injected_mv_count_l0 = 0;
    context_ptr->injected_mv_count_l1 = 0;
    context_ptr->injected_mv_count_bipred = 0;
#if FTR_PD2_REDUCE_MDS0
    context_ptr->inject_new_me = 1;
    context_ptr->inject_new_pme = 1;
    context_ptr->inject_new_warp = 1;
     uint8_t dc_cand_only_flag = context_ptr->dc_cand_only_flag;
#if CLN_CANDIDATE_ELEMINATION_CTR
     if(context_ptr->cand_elimination_ctrs.enabled)
#else
    if (context_ptr->eliminate_candidate_based_on_pme_me_results)
#endif
        eliminate_candidate_based_on_pme_me_results(context_ptr,
            pcs_ptr->parent_pcs_ptr->temporal_layer_index,
            pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag,
            &dc_cand_only_flag);
#endif
    //----------------------
    // Intra
    if (context_ptr->blk_geom->sq_size < 128) {
                inject_intra_candidates(
                    pcs_ptr,
                    context_ptr,
                    scs_ptr,
                    sb_ptr,
#if FTR_PD2_REDUCE_MDS0
                    dc_cand_only_flag,
#else
                    context_ptr->dc_cand_only_flag,
#endif
                &cand_total_cnt);
    }
       if (context_ptr->md_filter_intra_level > 0 && av1_filter_intra_allowed_bsize(scs_ptr->seq_header.filter_intra_level, context_ptr->blk_geom->bsize))
            inject_filter_intra_candidates(
                pcs_ptr,
                context_ptr,
                &cand_total_cnt);

    if (context_ptr->md_allow_intrabc)
        inject_intra_bc_candidates(
            pcs_ptr,
            context_ptr,
            scs_ptr,
            context_ptr->blk_ptr,
            &cand_total_cnt
        );
    if (context_ptr->md_palette_level) {
#if !CLN_MD_CANDS
    //can be removed later if need be
    for (uint16_t i = 0; i < cand_total_cnt; i++) {
        assert(context_ptr->fast_candidate_array[i].palette_info == NULL);
    }
#endif
    if (svt_av1_allow_palette(context_ptr->md_palette_level, context_ptr->blk_geom->bsize)) {
        inject_palette_candidates(
            pcs_ptr,
            context_ptr,
            &cand_total_cnt);
    }
#if !CLN_MD_CANDS
    for (uint16_t i = 0; i < cand_total_cnt; i++) {
        assert(context_ptr->fast_candidate_array[i].palette_info == NULL ||
                context_ptr->fast_candidate_array[i].palette_info->pmi.palette_size[0] < 9);
        assert(context_ptr->fast_candidate_array[i].palette_info == NULL ||
                context_ptr->fast_candidate_array[i].palette_info->pmi.palette_size[1] == 0);
    }
#endif
    }
    if (slice_type != I_SLICE && context_ptr->inject_inter_candidates) {
            inject_inter_candidates(
                pcs_ptr,
                context_ptr,
                scs_ptr,
                sb_ptr,
                &cand_total_cnt);
    }
    // For I_SLICE, DC is always injected, and therefore there is no a risk of no candidates @ md_syage_0()
    // For non I_SLICE, there is a risk of no candidates @ md_stage_0() because of the INTER candidates pruning techniques
    if (slice_type != I_SLICE && cand_total_cnt == 0) {
        inject_zz_backup_candidate(
            context_ptr,
            &cand_total_cnt);
    }
    *candidate_total_count_ptr = cand_total_cnt;


#if CLN_MD_CAND_BUFF
    for (uint32_t index = 0; index < MIN((*candidate_total_count_ptr + CAND_CLASS_TOTAL), context_ptr->max_nics); ++index)
#else
    for (uint32_t index = 0; index < MIN((*candidate_total_count_ptr + CAND_CLASS_TOTAL), MAX_NFL_BUFF_Y); ++index)
#endif
        context_ptr->fast_cost_array[index] = MAX_CU_COST;
#if !CLN_MD_CANDS
    CandClass  cand_class_it;
#endif
    memset(context_ptr->md_stage_0_count, 0, CAND_CLASS_TOTAL * sizeof(uint32_t));

#if CLN_MD_CANDS
    for (uint32_t cand_i = 0; cand_i < cand_total_cnt; cand_i++) {
#else
    uint32_t cand_i;
    for (cand_i = 0; cand_i < cand_total_cnt; cand_i++)
    {
#endif
        ModeDecisionCandidate * cand_ptr = &context_ptr->fast_candidate_array[cand_i];

        if (cand_ptr->type == INTRA_MODE) {
            // Intra prediction
                  if (cand_ptr->palette_info == NULL ||
                          cand_ptr->palette_info->pmi.palette_size[0] == 0) {
                    cand_ptr->cand_class = CAND_CLASS_0;
                    context_ptr->md_stage_0_count[CAND_CLASS_0]++;
                  }
                  else {
                      // Palette Prediction
                     cand_ptr->cand_class = CAND_CLASS_3;
                     context_ptr->md_stage_0_count[CAND_CLASS_3]++;
                  }
        }
#if CLN_MD_CANDS
        else { // INTER
#if TUNE_M10_MERGE_INTER_CLASSES
            if (cand_ptr->pred_mode == NEWMV || cand_ptr->pred_mode == NEW_NEWMV || context_ptr->merge_inter_classes) {
#else
            if (cand_ptr->pred_mode == NEWMV || cand_ptr->pred_mode == NEW_NEWMV) {
#endif
                // MV Prediction
                cand_ptr->cand_class = CAND_CLASS_1;
                context_ptr->md_stage_0_count[CAND_CLASS_1]++;
            }
            else {
                //MVP Prediction
                cand_ptr->cand_class = CAND_CLASS_2;
                context_ptr->md_stage_0_count[CAND_CLASS_2]++;
            }

            //check if final MV is within AV1 limits
            if (cand_ptr->prediction_direction[0] == UNI_PRED_LIST_0 ||
                cand_ptr->prediction_direction[0] == BI_PRED)
                check_mv_validity(cand_ptr->motion_vector_xl0,
                    cand_ptr->motion_vector_yl0, 0);

            if (cand_ptr->prediction_direction[0] == UNI_PRED_LIST_1 ||
                cand_ptr->prediction_direction[0] == BI_PRED)
                check_mv_validity(cand_ptr->motion_vector_xl1,
                    cand_ptr->motion_vector_yl1, 0);

        }
    }
#else
#if CLN_MD_CANDS
        else if (cand_ptr->pred_mode == NEWMV || cand_ptr->pred_mode == NEW_NEWMV) {
#else
        else  if (cand_ptr->is_new_mv) {
#endif
            // MV Prediction
            cand_ptr->cand_class = CAND_CLASS_1;
            context_ptr->md_stage_0_count[CAND_CLASS_1]++;
        }
        else {
            //MVP Prediction
            cand_ptr->cand_class = CAND_CLASS_2;
            context_ptr->md_stage_0_count[CAND_CLASS_2]++;
        }
    }

    uint32_t fast_accum = 0;
    for (cand_class_it = CAND_CLASS_0; cand_class_it < CAND_CLASS_TOTAL; cand_class_it++) {
        fast_accum += context_ptr->md_stage_0_count[cand_class_it];
    }
    assert(fast_accum == cand_total_cnt);

    //check if final MV is within AV1 limits
    for (cand_i = 0; cand_i < cand_total_cnt; cand_i++)
    {
        ModeDecisionCandidate * cand_ptr = &context_ptr->fast_candidate_array[cand_i];

        if (cand_ptr->type == INTER_MODE) {
            if (cand_ptr->prediction_direction[0] == UNI_PRED_LIST_0 ||
                cand_ptr->prediction_direction[0] == BI_PRED)
                check_mv_validity(cand_ptr->motion_vector_xl0,
                    cand_ptr->motion_vector_yl0, 0);

            if (cand_ptr->prediction_direction[0] == UNI_PRED_LIST_1 ||
                cand_ptr->prediction_direction[0] == BI_PRED)
                check_mv_validity(cand_ptr->motion_vector_xl1,
                    cand_ptr->motion_vector_yl1, 0);

        }
    }
#endif
    return EB_ErrorNone;
}

/***************************************
* Full Mode Decision
***************************************/
uint32_t product_full_mode_decision(
    struct ModeDecisionContext *context_ptr,
    BlkStruct *blk_ptr,
    ModeDecisionCandidateBuffer **buffer_ptr_array,
    uint32_t candidate_total_count,
    uint32_t *best_candidate_index_array)
{
    uint64_t                  lowest_cost = 0xFFFFFFFFFFFFFFFFull;
    uint32_t                  lowest_cost_index = 0;

    ModeDecisionCandidate       *candidate_ptr;

    lowest_cost_index = best_candidate_index_array[0];

    // Find the candidate with the lowest cost
    for (uint32_t i = 0; i < candidate_total_count; ++i) {
        uint32_t cand_index = best_candidate_index_array[i];

        if (*(buffer_ptr_array[cand_index]->full_cost_ptr) < lowest_cost) {
            lowest_cost_index = cand_index;
            lowest_cost = *(buffer_ptr_array[cand_index]->full_cost_ptr);
        }
    }
    if (context_ptr->pd_pass == PD_PASS_2) {
        blk_ptr->total_rate = buffer_ptr_array[lowest_cost_index]->candidate_ptr->total_rate;
    }

    candidate_ptr = buffer_ptr_array[lowest_cost_index]->candidate_ptr;
    if (context_ptr->blk_lambda_tuning){
        // When lambda tuning is on, lambda of each block is set separately, however at interdepth decision the sb lambda is used
        uint32_t full_lambda = context_ptr->hbd_mode_decision ?
            context_ptr->full_sb_lambda_md[EB_10_BIT_MD] :
            context_ptr->full_sb_lambda_md[EB_8_BIT_MD];
        context_ptr->md_local_blk_unit[blk_ptr->mds_idx].cost =
            RDCOST(full_lambda,
                buffer_ptr_array[lowest_cost_index]->candidate_ptr->total_rate,
                ((uint64_t)buffer_ptr_array[lowest_cost_index]->candidate_ptr->full_distortion));
        context_ptr->md_local_blk_unit[blk_ptr->mds_idx].default_cost = context_ptr->md_local_blk_unit[blk_ptr->mds_idx].cost;
    }
    else {
        context_ptr->md_local_blk_unit[blk_ptr->mds_idx].cost = *(buffer_ptr_array[lowest_cost_index]->full_cost_ptr);
        context_ptr->md_local_blk_unit[blk_ptr->mds_idx].default_cost = *(buffer_ptr_array[lowest_cost_index]->full_cost_ptr);
    }
#if FTR_PD2_BLOCK_REDUCTION
    context_ptr->md_local_blk_unit[blk_ptr->mds_idx].count_non_zero_coeffs = candidate_ptr->count_non_zero_coeffs;
#endif
    context_ptr->md_ep_pipe_sb[blk_ptr->mds_idx].merge_cost = *buffer_ptr_array[lowest_cost_index]->full_cost_merge_ptr;
    context_ptr->md_ep_pipe_sb[blk_ptr->mds_idx].skip_cost = *buffer_ptr_array[lowest_cost_index]->full_cost_skip_ptr;

    context_ptr->md_local_blk_unit[blk_ptr->mds_idx].full_distortion = (uint32_t)buffer_ptr_array[lowest_cost_index]->candidate_ptr->full_distortion;
    blk_ptr->prediction_mode_flag = candidate_ptr->type;
    blk_ptr->tx_depth = candidate_ptr->tx_depth;
    blk_ptr->skip_flag = candidate_ptr->skip_flag; // note, the skip flag is re-checked in the ENCDEC process
    blk_ptr->block_has_coeff = ((candidate_ptr->block_has_coeff) > 0) ? EB_TRUE : EB_FALSE;
#if  CLN_SB_DATA
    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].quantized_dc[1][0] = buffer_ptr_array[lowest_cost_index]->candidate_ptr->quantized_dc[1][0];
    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].quantized_dc[2][0] = buffer_ptr_array[lowest_cost_index]->candidate_ptr->quantized_dc[2][0];
#else
    blk_ptr->quantized_dc[1][0] = buffer_ptr_array[lowest_cost_index]->candidate_ptr->quantized_dc[1][0];
    blk_ptr->quantized_dc[2][0] = buffer_ptr_array[lowest_cost_index]->candidate_ptr->quantized_dc[2][0];
#endif
    context_ptr->md_local_blk_unit[blk_ptr->mds_idx].count_non_zero_coeffs = candidate_ptr->count_non_zero_coeffs;
    blk_ptr->use_intrabc = candidate_ptr->use_intrabc;
    if (blk_ptr->prediction_mode_flag == INTER_MODE && candidate_ptr->is_compound)
    {
        blk_ptr->interinter_comp.type = candidate_ptr->interinter_comp.type;
        blk_ptr->interinter_comp.mask_type = candidate_ptr->interinter_comp.mask_type;
        blk_ptr->interinter_comp.wedge_index = candidate_ptr->interinter_comp.wedge_index;
        blk_ptr->interinter_comp.wedge_sign = candidate_ptr->interinter_comp.wedge_sign;
        blk_ptr->compound_idx = candidate_ptr->compound_idx;
        blk_ptr->comp_group_idx = candidate_ptr->comp_group_idx;
        if (blk_ptr->interinter_comp.type == COMPOUND_AVERAGE){
            if (blk_ptr->comp_group_idx != 0 || blk_ptr->compound_idx != 1)
                SVT_LOG("Error: Compound combination not allowed\n");
        }
    }
    blk_ptr->is_interintra_used          = candidate_ptr->is_interintra_used;
    blk_ptr->interintra_mode             = candidate_ptr->interintra_mode;
    blk_ptr->use_wedge_interintra        = candidate_ptr->use_wedge_interintra;
    blk_ptr->interintra_wedge_index      = candidate_ptr->interintra_wedge_index;

    // Set the PU level variables
    blk_ptr->interp_filters = candidate_ptr->interp_filters;
    {
        PredictionUnit       *pu_ptr = blk_ptr->prediction_unit_array;
        if (blk_ptr->prediction_mode_flag == INTRA_MODE)
        {
            blk_ptr->filter_intra_mode= candidate_ptr->filter_intra_mode;
            pu_ptr->is_directional_mode_flag = candidate_ptr->is_directional_mode_flag;
            pu_ptr->angle_delta[PLANE_TYPE_Y] = candidate_ptr->angle_delta[PLANE_TYPE_Y];

            pu_ptr->cfl_alpha_idx = candidate_ptr->cfl_alpha_idx;
            pu_ptr->cfl_alpha_signs = candidate_ptr->cfl_alpha_signs;

            pu_ptr->intra_chroma_mode = candidate_ptr->intra_chroma_mode;
            pu_ptr->is_directional_chroma_mode_flag = candidate_ptr->is_directional_chroma_mode_flag;
            pu_ptr->angle_delta[PLANE_TYPE_UV] = candidate_ptr->angle_delta[PLANE_TYPE_UV];
        }
        if (blk_ptr->prediction_mode_flag == INTRA_MODE)
        {
            if (candidate_ptr->palette_info)
                memcpy(&blk_ptr->palette_info.pmi, &candidate_ptr->palette_info->pmi, sizeof(PaletteModeInfo));
            else
                memset(&blk_ptr->palette_info.pmi, 0, sizeof(PaletteModeInfo));
            if (svt_av1_allow_palette(context_ptr->md_palette_level, context_ptr->blk_geom->bsize))
            {
               if (candidate_ptr->palette_info)
                   memcpy(blk_ptr->palette_info.color_idx_map, candidate_ptr->palette_info->color_idx_map, MAX_PALETTE_SQUARE);
               else
                   memset(blk_ptr->palette_info.color_idx_map, 0, MAX_PALETTE_SQUARE);
            }
        }
        else {
            blk_ptr->palette_info.pmi.palette_size[0] = blk_ptr->palette_info.pmi.palette_size[1] = 0;
        }
        // Inter Prediction
        pu_ptr->inter_pred_direction_index = candidate_ptr->prediction_direction[0];
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].skip_mode_allowed = candidate_ptr->skip_mode_allowed;
        if (blk_ptr->prediction_mode_flag != INTER_MODE && blk_ptr->use_intrabc == 0)
        {
            pu_ptr->inter_pred_direction_index = 0x03;
            context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].skip_mode_allowed = EB_FALSE;
        }
        pu_ptr->mv[REF_LIST_0].x = 0;
        pu_ptr->mv[REF_LIST_0].y = 0;

        pu_ptr->mv[REF_LIST_1].x = 0;
        pu_ptr->mv[REF_LIST_1].y = 0;

        blk_ptr->pred_mode = candidate_ptr->pred_mode;
        blk_ptr->drl_index = candidate_ptr->drl_index;
#if !CLN_MD_CANDS
        pu_ptr->inter_mode = candidate_ptr->inter_mode;
#endif
        pu_ptr->is_compound = candidate_ptr->is_compound;
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].compound_idx = candidate_ptr->compound_idx;
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].interinter_comp = candidate_ptr->interinter_comp;
        pu_ptr->ref_frame_type = candidate_ptr->ref_frame_type;
#if !CLN_MD_CANDS
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].ref_frame_index_l0 = candidate_ptr->ref_frame_index_l0;
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].ref_frame_index_l1 = candidate_ptr->ref_frame_index_l1;
#endif
        if (pu_ptr->inter_pred_direction_index == UNI_PRED_LIST_0)
        {
            //svt_memcpy(&pu_ptr->mv[REF_LIST_0].x,&candidate_ptr->mvs_l0,4);
            pu_ptr->mv[REF_LIST_0].x = candidate_ptr->motion_vector_xl0;
            pu_ptr->mv[REF_LIST_0].y = candidate_ptr->motion_vector_yl0;
        }

        if (pu_ptr->inter_pred_direction_index == UNI_PRED_LIST_1)
        {
            //svt_memcpy(&pu_ptr->mv[REF_LIST_1].x,&candidate_ptr->mvs_l1,4);
            pu_ptr->mv[REF_LIST_1].x = candidate_ptr->motion_vector_xl1;
            pu_ptr->mv[REF_LIST_1].y = candidate_ptr->motion_vector_yl1;
        }

        if (pu_ptr->inter_pred_direction_index == BI_PRED)
        {
            //svt_memcpy(&pu_ptr->mv[REF_LIST_0].x,&candidate_ptr->mvs,8);
            pu_ptr->mv[REF_LIST_0].x = candidate_ptr->motion_vector_xl0;
            pu_ptr->mv[REF_LIST_0].y = candidate_ptr->motion_vector_yl0;
            pu_ptr->mv[REF_LIST_1].x = candidate_ptr->motion_vector_xl1;
            pu_ptr->mv[REF_LIST_1].y = candidate_ptr->motion_vector_yl1;
        }
        if (pu_ptr->inter_pred_direction_index == UNI_PRED_LIST_0) {
            blk_ptr->predmv[0].as_mv.col = candidate_ptr->motion_vector_pred_x[REF_LIST_0];
            blk_ptr->predmv[0].as_mv.row = candidate_ptr->motion_vector_pred_y[REF_LIST_0];
        }
        else if (pu_ptr->inter_pred_direction_index == UNI_PRED_LIST_1) {
            blk_ptr->predmv[0].as_mv.col = candidate_ptr->motion_vector_pred_x[REF_LIST_1];
            blk_ptr->predmv[0].as_mv.row = candidate_ptr->motion_vector_pred_y[REF_LIST_1];
        }
        else if (pu_ptr->inter_pred_direction_index == BI_PRED) {
            blk_ptr->predmv[0].as_mv.col = candidate_ptr->motion_vector_pred_x[REF_LIST_0];
            blk_ptr->predmv[0].as_mv.row = candidate_ptr->motion_vector_pred_y[REF_LIST_0];
            blk_ptr->predmv[1].as_mv.col = candidate_ptr->motion_vector_pred_x[REF_LIST_1];
            blk_ptr->predmv[1].as_mv.row = candidate_ptr->motion_vector_pred_y[REF_LIST_1];
        }
        pu_ptr->overlappable_neighbors[0] = context_ptr->blk_ptr->prediction_unit_array[0].overlappable_neighbors[0];
        pu_ptr->overlappable_neighbors[1] = context_ptr->blk_ptr->prediction_unit_array[0].overlappable_neighbors[1];
        pu_ptr->motion_mode = candidate_ptr->motion_mode;
        pu_ptr->num_proj_ref = candidate_ptr->num_proj_ref;
        if (pu_ptr->motion_mode == WARPED_CAUSAL) {
            svt_memcpy(&context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].wm_params_l0, &candidate_ptr->wm_params_l0, sizeof(EbWarpedMotionParams));
            svt_memcpy(&context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].wm_params_l1, &candidate_ptr->wm_params_l1, sizeof(EbWarpedMotionParams));
        }
    }

    uint32_t txb_itr;
    uint32_t txb_index;
    uint32_t tu_total_count;
#if !OPT_INIT
    uint32_t cu_size_log2 = context_ptr->cu_size_log2;
#endif
    tu_total_count = context_ptr->blk_geom->txb_count[blk_ptr->tx_depth];
    txb_index = 0;
    txb_itr = 0;
#if NO_ENCDEC
    int32_t txb_1d_offset = 0, txb_1d_offset_uv = 0;

    blk_ptr->block_has_coeff = 0;
#endif

    //blk_ptr->forceSmallTu = candidate_ptr->forceSmallTu;

    // Set TU
    do {
        TransformUnit *txb_ptr = &blk_ptr->txb_array[txb_index];
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].y_has_coeff[txb_index] = (EbBool)(((candidate_ptr->y_has_coeff) & (1 << txb_index)) > 0);
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].u_has_coeff[txb_index] = (EbBool)(((candidate_ptr->u_has_coeff) & (1 << txb_index)) > 0);
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].v_has_coeff[txb_index] = (EbBool)(((candidate_ptr->v_has_coeff) & (1 << txb_index)) > 0);
        txb_ptr->transform_type[PLANE_TYPE_Y] = candidate_ptr->transform_type[txb_index];
        txb_ptr->transform_type[PLANE_TYPE_UV] = candidate_ptr->transform_type_uv;
#if  CLN_SB_DATA
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].quantized_dc[0][txb_index] = candidate_ptr->quantized_dc[0][txb_index];
#else
        blk_ptr->quantized_dc[0][txb_index] = candidate_ptr->quantized_dc[0][txb_index];
#endif
#if NO_ENCDEC

        if (context_ptr->blk_geom->has_uv) {
            blk_ptr->block_has_coeff |= txb_ptr->y_has_coeff;
            blk_ptr->block_has_coeff |= txb_ptr->u_has_coeff;
            blk_ptr->block_has_coeff |= txb_ptr->v_has_coeff;
        }
        else
            blk_ptr->block_has_coeff |= txb_ptr->y_has_coeff;
        blk_ptr->cand_buff_index = lowest_cost_index;

        blk_ptr->skip_flag = 0;   //SKIP is turned OFF for this case!!
        txb_ptr->nz_coef_count[0] = candidate_ptr->eob[0][txb_index];
        txb_ptr->nz_coef_count[1] = candidate_ptr->eob[1][txb_index];
        txb_ptr->nz_coef_count[2] = candidate_ptr->eob[2][txb_index];

        if (pu_ptr->inter_pred_direction_index == UNI_PRED_LIST_0) {
            blk_ptr->predmv[0].as_mv.col = candidate_ptr->motion_vector_pred_x[REF_LIST_0];
            blk_ptr->predmv[0].as_mv.row = candidate_ptr->motion_vector_pred_y[REF_LIST_0];
        }
        else if (pu_ptr->inter_pred_direction_index == UNI_PRED_LIST_1) {
            blk_ptr->predmv[0].as_mv.col = candidate_ptr->motion_vector_pred_x[REF_LIST_1];
            blk_ptr->predmv[0].as_mv.row = candidate_ptr->motion_vector_pred_y[REF_LIST_1];
        }
        else if (pu_ptr->inter_pred_direction_index == BI_PRED) {
            blk_ptr->predmv[0].as_mv.col = candidate_ptr->motion_vector_pred_x[REF_LIST_0];
            blk_ptr->predmv[0].as_mv.row = candidate_ptr->motion_vector_pred_y[REF_LIST_0];
            blk_ptr->predmv[1].as_mv.col = candidate_ptr->motion_vector_pred_x[REF_LIST_1];
            blk_ptr->predmv[1].as_mv.row = candidate_ptr->motion_vector_pred_y[REF_LIST_1];
        }
#endif
#if NO_ENCDEC
        //copy coeff
        {
            uint32_t  bwidth = context_ptr->blk_geom->tx_width[txb_itr] < 64 ? context_ptr->blk_geom->tx_width[txb_itr] : 32;
            uint32_t  bheight = context_ptr->blk_geom->tx_height[txb_itr] < 64 ? context_ptr->blk_geom->tx_height[txb_itr] : 32;

            int32_t* src_ptr = &(((int32_t*)buffer_ptr_array[lowest_cost_index]->residual_quant_coeff_ptr->buffer_y)[txb_1d_offset]);
            int32_t* dst_ptr = &(((int32_t*)context_ptr->blk_ptr->coeff_tmp->buffer_y)[txb_1d_offset]);

            uint32_t j;

            for (j = 0; j < bheight; j++)
                svt_memcpy(dst_ptr + j * bwidth, src_ptr + j * bwidth, bwidth * sizeof(int32_t));
            if (context_ptr->blk_geom->has_uv)
            {
                // Cb
                bwidth = context_ptr->blk_geom->tx_width_uv[txb_itr];
                bheight = context_ptr->blk_geom->tx_height_uv[txb_itr];

                src_ptr = &(((int32_t*)buffer_ptr_array[lowest_cost_index]->residual_quant_coeff_ptr->buffer_cb)[txb_1d_offset_uv]);
                dst_ptr = &(((int32_t*)context_ptr->blk_ptr->coeff_tmp->buffer_cb)[txb_1d_offset_uv]);

                for (j = 0; j < bheight; j++)
                    svt_memcpy(dst_ptr + j * bwidth, src_ptr + j * bwidth, bwidth * sizeof(int32_t));
                src_ptr = &(((int32_t*)buffer_ptr_array[lowest_cost_index]->residual_quant_coeff_ptr->buffer_cr)[txb_1d_offset_uv]);
                dst_ptr = &(((int32_t*)context_ptr->blk_ptr->coeff_tmp->buffer_cr)[txb_1d_offset_uv]);

                for (j = 0; j < bheight; j++)
                    svt_memcpy(dst_ptr + j * bwidth, src_ptr + j * bwidth, bwidth * sizeof(int32_t));
            }

            txb_1d_offset += context_ptr->blk_geom->tx_width[txb_itr] * context_ptr->blk_geom->tx_height[txb_itr];
            if (context_ptr->blk_geom->has_uv)
                txb_1d_offset_uv += context_ptr->blk_geom->tx_width_uv[txb_itr] * context_ptr->blk_geom->tx_height_uv[txb_itr];
        }

#endif

        ++txb_index;
        ++txb_itr;
    } while (txb_itr < tu_total_count);
#if !OPT_INIT
    UNUSED(cu_size_log2);
#endif
    return lowest_cost_index;
}
uint32_t get_blk_tuned_full_lambda(struct ModeDecisionContext *context_ptr, PictureControlSet *pcs_ptr,
        uint32_t pic_full_lambda) {
    PictureParentControlSet *ppcs_ptr = pcs_ptr->parent_pcs_ptr;
    Av1Common *cm = ppcs_ptr->av1_cm;

    BlockSize bsize = context_ptr->blk_geom->bsize;
    const int bsize_base = BLOCK_16X16;
    const int num_mi_w = mi_size_wide[bsize_base];
    const int num_mi_h = mi_size_high[bsize_base];
    const int num_cols = (cm->mi_cols + num_mi_w - 1) / num_mi_w;
    const int num_rows = (cm->mi_rows + num_mi_h - 1) / num_mi_h;
    const int num_bcols = (mi_size_wide[bsize] + num_mi_w - 1) / num_mi_w;
    const int num_brows = (mi_size_high[bsize] + num_mi_h - 1) / num_mi_h;
    int mi_row = context_ptr->blk_origin_y / 4;
    int mi_col = context_ptr->blk_origin_x / 4;
    int row, col;
    double base_block_count = 0.0;
    double geom_mean_of_scale = 0.0;
    for (row = mi_row / num_mi_w;
            row < num_rows && row < mi_row / num_mi_w + num_brows; ++row) {
        for (col = mi_col / num_mi_h;
                col < num_cols && col < mi_col / num_mi_h + num_bcols; ++col) {
            const int index = row * num_cols + col;
            geom_mean_of_scale += log(ppcs_ptr->tpl_sb_rdmult_scaling_factors[index]);
            base_block_count += 1.0;
        }
    }
    assert(base_block_count > 0);
    geom_mean_of_scale = exp(geom_mean_of_scale / base_block_count);
    uint32_t new_full_lambda = (uint32_t)((double)pic_full_lambda * geom_mean_of_scale + 0.5);
    new_full_lambda = AOMMAX(new_full_lambda, 0);
    return new_full_lambda;
}

void set_tuned_blk_lambda(struct ModeDecisionContext *context_ptr, PictureControlSet *pcs_ptr) {
    context_ptr->full_lambda_md[EB_8_BIT_MD] =
        get_blk_tuned_full_lambda(context_ptr,
            pcs_ptr,
            context_ptr->enc_dec_context_ptr->pic_full_lambda[EB_8_BIT_MD]);

    context_ptr->full_lambda_md[EB_10_BIT_MD] =
        get_blk_tuned_full_lambda(context_ptr,
            pcs_ptr,
            context_ptr->enc_dec_context_ptr->pic_full_lambda[EB_10_BIT_MD]);
    context_ptr->fast_lambda_md[EB_8_BIT_MD] =
        get_blk_tuned_full_lambda(context_ptr,
            pcs_ptr,
            context_ptr->enc_dec_context_ptr->pic_fast_lambda[EB_8_BIT_MD]);

    context_ptr->fast_lambda_md[EB_10_BIT_MD] =
        get_blk_tuned_full_lambda(context_ptr,
            pcs_ptr,
            context_ptr->enc_dec_context_ptr->pic_fast_lambda[EB_10_BIT_MD]);
}
