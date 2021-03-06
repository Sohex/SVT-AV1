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

#include <stdlib.h>

#include "EbDefinitions.h"
#include "EbUtility.h"
#include "EbTransformUnit.h"
#include "EbRateDistortionCost.h"
#include "EbFullLoop.h"
#include "EbPictureOperators.h"
#include "EbModeDecisionProcess.h"
#include "EbTransforms.h"
#include "EbMotionEstimation.h"
#include "aom_dsp_rtcd.h"
#include "EbCodingLoop.h"
#include "EbLog.h"
#include "EbCommonUtils.h"
#include "EbResize.h"
#include "mv.h"
#include "mcomp.h"
#include "av1me.h"
#include "limits.h"
void check_mv_validity(int16_t x_mv, int16_t y_mv, uint8_t need_shift);
#define DIVIDE_AND_ROUND(x, y) (((x) + ((y) >> 1)) / (y))
void svt_init_mv_cost_params(MV_COST_PARAMS *mv_cost_params, ModeDecisionContext *context_ptr,
                             const MV *ref_mv, uint8_t base_q_idx, uint32_t rdmult,
                             uint8_t hbd_mode_decision);
int  fp_mv_err_cost(const MV *mv, const MV_COST_PARAMS *mv_cost_params);
extern AomVarianceFnPtr mefn_ptr[BlockSizeS_ALL];
EbErrorType generate_md_stage_0_cand(SuperBlock *sb_ptr, ModeDecisionContext *context_ptr,
                                     uint32_t *         fast_candidate_total_count,
                                     PictureControlSet *pcs_ptr);

int16_t svt_av1_dc_quant_qtx(int32_t qindex, int32_t delta, AomBitDepth bit_depth);

static INLINE int is_interintra_allowed_bsize(const BlockSize bsize) {
    return (bsize >= BLOCK_8X8) && (bsize <= BLOCK_32X32);
}
void precompute_intra_pred_for_inter_intra(PictureControlSet *  pcs_ptr,
                                           ModeDecisionContext *context_ptr);

int svt_av1_allow_palette(int allow_palette, BlockSize sb_type);

/*******************************************
* set Penalize Skip Flag
*
* Summary: Set the penalize_skipflag to true
* When there is luminance/chrominance change
* or in noisy clip with low motion at meduim
* varince area
*
*******************************************/

const EbPredictionFunc svt_product_prediction_fun_table[3] = {
    NULL, inter_pu_prediction_av1, svt_av1_intra_prediction_cl};

const EbFastCostFunc av1_product_fast_cost_func_table[3] = {
    NULL,
    av1_inter_fast_cost, /*INTER */
    av1_intra_fast_cost /*INTRA */
};

const EbAv1FullCostFunc svt_av1_product_full_cost_func_table[3] = {
    NULL,
    av1_inter_full_cost, /*INTER */
    av1_intra_full_cost /*INTRA */
};

/***************************************************
* Update Recon Samples Neighbor Arrays
***************************************************/
void mode_decision_update_neighbor_arrays(PictureControlSet *  pcs_ptr,
                                          ModeDecisionContext *context_ptr, uint32_t index_mds) {
    uint32_t bwdith  = context_ptr->blk_geom->bwidth;
    uint32_t bheight = context_ptr->blk_geom->bheight;

    uint32_t origin_x        = context_ptr->blk_origin_x;
    uint32_t origin_y        = context_ptr->blk_origin_y;
    uint32_t blk_origin_x_uv = context_ptr->round_origin_x >> 1;
    uint32_t blk_origin_y_uv = context_ptr->round_origin_y >> 1;
    uint32_t bwdith_uv       = context_ptr->blk_geom->bwidth_uv;
    uint32_t bwheight_uv     = context_ptr->blk_geom->bheight_uv;

    uint8_t mode_type       = context_ptr->blk_ptr->prediction_mode_flag;
    uint8_t intra_luma_mode = (uint8_t)context_ptr->blk_ptr->pred_mode;
#if !CLN_MDC_CTX
    uint8_t chroma_mode = (uint8_t)context_ptr->blk_ptr->prediction_unit_array->intra_chroma_mode;
#endif
    uint8_t skip_flag = (uint8_t)context_ptr->blk_ptr->skip_flag;

    context_ptr->mv_unit.pred_direction = (uint8_t)(
        context_ptr->md_blk_arr_nsq[index_mds].prediction_unit_array[0].inter_pred_direction_index);
    context_ptr->mv_unit.mv[REF_LIST_0].mv_union =
        context_ptr->md_blk_arr_nsq[index_mds].prediction_unit_array[0].mv[REF_LIST_0].mv_union;
    context_ptr->mv_unit.mv[REF_LIST_1].mv_union =
        context_ptr->md_blk_arr_nsq[index_mds].prediction_unit_array[0].mv[REF_LIST_1].mv_union;
#if !CLN_MDC_CTX
    uint8_t inter_pred_direction_index =
        (uint8_t)context_ptr->blk_ptr->prediction_unit_array->inter_pred_direction_index;
#endif
    uint8_t ref_frame_type = (uint8_t)context_ptr->blk_ptr->prediction_unit_array[0].ref_frame_type;
    int32_t is_inter       = (context_ptr->blk_ptr->prediction_mode_flag == INTER_MODE ||
                        context_ptr->blk_ptr->use_intrabc)
              ? EB_TRUE
              : EB_FALSE;

    uint16_t tile_idx = context_ptr->tile_index;
    if (context_ptr->interpolation_search_level != IFS_OFF)
        neighbor_array_unit_mode_write32(context_ptr->interpolation_type_neighbor_array,
                                         context_ptr->blk_ptr->interp_filters,
                                         origin_x,
                                         origin_y,
                                         bwdith,
                                         bheight,
                                         NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

    {
#if OPT_D2_COPIES
        if (!(context_ptr->pd_pass == PD_PASS_1 /*&& context_ptr->pred_depth_only*/)) {
#endif
        struct PartitionContext partition;
        partition.above = partition_context_lookup[context_ptr->blk_geom->bsize].above;
        partition.left  = partition_context_lookup[context_ptr->blk_geom->bsize].left;

        neighbor_array_unit_mode_write(context_ptr->leaf_partition_neighbor_array,
                                       (uint8_t *)(&partition), // NaderM
                                       origin_x,
                                       origin_y,
                                       bwdith,
                                       bheight,
                                       NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
#if OPT_D2_COPIES
        }
#endif
        // Mode Type Update
        if (!context_ptr->shut_fast_rate)
            neighbor_array_unit_mode_write(context_ptr->mode_type_neighbor_array,
                                           &mode_type,
                                           origin_x,
                                           origin_y,
                                           bwdith,
                                           bheight,
                                           NEIGHBOR_ARRAY_UNIT_FULL_MASK);
        // Intra Luma Mode Update
        if (!context_ptr->shut_fast_rate)
            neighbor_array_unit_mode_write(context_ptr->intra_luma_mode_neighbor_array,
                                           &intra_luma_mode, //(uint8_t*)luma_mode,
                                           origin_x,
                                           origin_y,
                                           bwdith,
                                           bheight,
                                           NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
        if (!context_ptr->shut_skip_ctx_dc_sign_update) {
            uint16_t txb_count = context_ptr->blk_geom->txb_count[context_ptr->blk_ptr->tx_depth];
            for (uint8_t txb_itr = 0; txb_itr < txb_count; txb_itr++) {
#if CLN_SB_DATA
                uint8_t dc_sign_level_coeff =
                    (uint8_t)context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .quantized_dc[0][txb_itr];
#else
                uint8_t dc_sign_level_coeff = (int32_t)
                                                  context_ptr->blk_ptr->quantized_dc[0][txb_itr];
#endif
                neighbor_array_unit_mode_write(
                    context_ptr->luma_dc_sign_level_coeff_neighbor_array,
                    (uint8_t *)&dc_sign_level_coeff,
                    context_ptr->sb_origin_x +
                        context_ptr->blk_geom
                            ->tx_org_x[is_inter][context_ptr->blk_ptr->tx_depth][txb_itr],
                    context_ptr->sb_origin_y +
                        context_ptr->blk_geom
                            ->tx_org_y[is_inter][context_ptr->blk_ptr->tx_depth][txb_itr],
                    context_ptr->blk_geom->tx_width[context_ptr->blk_ptr->tx_depth][txb_itr],
                    context_ptr->blk_geom->tx_height[context_ptr->blk_ptr->tx_depth][txb_itr],
                    NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

                neighbor_array_unit_mode_write(
                    pcs_ptr->md_tx_depth_1_luma_dc_sign_level_coeff_neighbor_array
                        [MD_NEIGHBOR_ARRAY_INDEX][tile_idx],
                    (uint8_t *)&dc_sign_level_coeff,
                    context_ptr->sb_origin_x +
                        context_ptr->blk_geom
                            ->tx_org_x[is_inter][context_ptr->blk_ptr->tx_depth][txb_itr],
                    context_ptr->sb_origin_y +
                        context_ptr->blk_geom
                            ->tx_org_y[is_inter][context_ptr->blk_ptr->tx_depth][txb_itr],
                    context_ptr->blk_geom->tx_width[context_ptr->blk_ptr->tx_depth][txb_itr],
                    context_ptr->blk_geom->tx_height[context_ptr->blk_ptr->tx_depth][txb_itr],
                    NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
            }
        }
    }
#if !CLN_MDC_CTX
    if (!context_ptr->shut_fast_rate)
        if (context_ptr->blk_geom->has_uv) {
            // Intra Chroma Mode Update
            neighbor_array_unit_mode_write(context_ptr->intra_chroma_mode_neighbor_array,
                                           &chroma_mode,
                                           blk_origin_x_uv,
                                           blk_origin_y_uv,
                                           bwdith_uv,
                                           bwheight_uv,
                                           NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
        }
#endif
    if (!context_ptr->shut_fast_rate)
        neighbor_array_unit_mode_write(context_ptr->skip_flag_neighbor_array,
                                       &skip_flag,
                                       origin_x,
                                       origin_y,
                                       bwdith,
                                       bheight,
                                       NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
    if (!context_ptr->shut_skip_ctx_dc_sign_update)

        if (context_ptr->blk_geom->has_uv && context_ptr->chroma_level <= CHROMA_MODE_1) {
            //  Update chroma CB cbf and Dc context
            {
#if CLN_SB_DATA
                uint8_t dc_sign_level_coeff =
                    (uint8_t)context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .quantized_dc[1][0];
#else
                uint8_t dc_sign_level_coeff = (int32_t)context_ptr->blk_ptr->quantized_dc[1][0];
#endif
                neighbor_array_unit_mode_write(context_ptr->cb_dc_sign_level_coeff_neighbor_array,
                                               (uint8_t *)&dc_sign_level_coeff,
                                               blk_origin_x_uv,
                                               blk_origin_y_uv,
                                               bwdith_uv,
                                               bwheight_uv,
                                               NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
            }

            //  Update chroma CR cbf and Dc context
            {
#if CLN_SB_DATA
                uint8_t dc_sign_level_coeff =
                    (uint8_t)context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .quantized_dc[2][0];
#else
                uint8_t dc_sign_level_coeff = (int32_t)context_ptr->blk_ptr->quantized_dc[2][0];
#endif
                neighbor_array_unit_mode_write(context_ptr->cr_dc_sign_level_coeff_neighbor_array,
                                               (uint8_t *)&dc_sign_level_coeff,
                                               blk_origin_x_uv,
                                               blk_origin_y_uv,
                                               bwdith_uv,
                                               bwheight_uv,
                                               NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
            }
        }
#if OPT_D2_COPIES
    if (pcs_ptr->parent_pcs_ptr->frm_hdr.tx_mode == TX_MODE_SELECT) {
#endif
    uint8_t tx_size =
        tx_depth_to_tx_size[context_ptr->blk_ptr->tx_depth][context_ptr->blk_geom->bsize];
    uint8_t bw = tx_size_wide[tx_size];
    uint8_t bh = tx_size_high[tx_size];

    neighbor_array_unit_mode_write(context_ptr->txfm_context_array,
                                   &bw,
                                   origin_x,
                                   origin_y,
                                   bwdith,
                                   bheight,
                                   NEIGHBOR_ARRAY_UNIT_TOP_MASK);

    neighbor_array_unit_mode_write(context_ptr->txfm_context_array,
                                   &bh,
                                   origin_x,
                                   origin_y,
                                   bwdith,
                                   bheight,
                                   NEIGHBOR_ARRAY_UNIT_LEFT_MASK);
#if OPT_D2_COPIES
    }
#endif
#if !CLN_MDC_CTX
    // Update the Inter Pred Type Neighbor Array
    if (!context_ptr->shut_fast_rate)
        neighbor_array_unit_mode_write(context_ptr->inter_pred_dir_neighbor_array,
                                       &inter_pred_direction_index,
                                       origin_x,
                                       origin_y,
                                       bwdith,
                                       bheight,
                                       NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
#endif
    // Update the refFrame Type Neighbor Array
    if (!context_ptr->shut_fast_rate)
        neighbor_array_unit_mode_write(context_ptr->ref_frame_type_neighbor_array,
                                       &ref_frame_type,
                                       origin_x,
                                       origin_y,
                                       bwdith,
                                       bheight,
                                       NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
    if (!context_ptr->skip_intra) {
        if (!context_ptr->hbd_mode_decision) {
            update_recon_neighbor_array(
                context_ptr->luma_recon_neighbor_array,
                context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                    .neigh_top_recon[0],
                context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                    .neigh_left_recon[0],
                origin_x,
                origin_y,
                context_ptr->blk_geom->bwidth,
                context_ptr->blk_geom->bheight);
            if (context_ptr->md_tx_size_search_mode) {
                update_recon_neighbor_array(
                    pcs_ptr->md_tx_depth_1_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX]
                                                                    [tile_idx],
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_top_recon[0],
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_left_recon[0],
                    origin_x,
                    origin_y,
                    context_ptr->blk_geom->bwidth,
                    context_ptr->blk_geom->bheight);
                update_recon_neighbor_array(
                    pcs_ptr->md_tx_depth_2_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX]
                                                                    [tile_idx],
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_top_recon[0],
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_left_recon[0],
                    origin_x,
                    origin_y,
                    context_ptr->blk_geom->bwidth,
                    context_ptr->blk_geom->bheight);
            }

            if (context_ptr->blk_geom->has_uv && context_ptr->chroma_level <= CHROMA_MODE_1) {
                update_recon_neighbor_array(
                    context_ptr->cb_recon_neighbor_array,
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_top_recon[1],
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_left_recon[1],
                    blk_origin_x_uv,
                    blk_origin_y_uv,
                    bwdith_uv,
                    bwheight_uv);
                update_recon_neighbor_array(
                    context_ptr->cr_recon_neighbor_array,
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_top_recon[2],
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_left_recon[2],
                    blk_origin_x_uv,
                    blk_origin_y_uv,
                    bwdith_uv,
                    bwheight_uv);
            }
        } else {
            update_recon_neighbor_array16bit(
                context_ptr->luma_recon_neighbor_array16bit,
                context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                    .neigh_top_recon_16bit[0],
                context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                    .neigh_left_recon_16bit[0],
                origin_x,
                origin_y,
                context_ptr->blk_geom->bwidth,
                context_ptr->blk_geom->bheight);
            if (context_ptr->md_tx_size_search_mode) {
                update_recon_neighbor_array16bit(
                    pcs_ptr->md_tx_depth_1_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX]
                                                                         [tile_idx],
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_top_recon_16bit[0],
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_left_recon_16bit[0],
                    origin_x,
                    origin_y,
                    context_ptr->blk_geom->bwidth,
                    context_ptr->blk_geom->bheight);
                update_recon_neighbor_array16bit(
                    pcs_ptr->md_tx_depth_2_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX]
                                                                         [tile_idx],
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_top_recon_16bit[0],
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_left_recon_16bit[0],
                    origin_x,
                    origin_y,
                    context_ptr->blk_geom->bwidth,
                    context_ptr->blk_geom->bheight);
            }

            if (context_ptr->blk_geom->has_uv && context_ptr->chroma_level <= CHROMA_MODE_1) {
                update_recon_neighbor_array16bit(
                    context_ptr->cb_recon_neighbor_array16bit,
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_top_recon_16bit[1],
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_left_recon_16bit[1],
                    blk_origin_x_uv,
                    blk_origin_y_uv,
                    bwdith_uv,
                    bwheight_uv);
                update_recon_neighbor_array16bit(
                    context_ptr->cr_recon_neighbor_array16bit,
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_top_recon_16bit[2],
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .neigh_left_recon_16bit[2],
                    blk_origin_x_uv,
                    blk_origin_y_uv,
                    bwdith_uv,
                    bwheight_uv);
            }
        }
    }
    return;
}

void copy_neighbour_arrays(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                           uint32_t src_idx, uint32_t dst_idx, uint32_t blk_mds, uint32_t sb_org_x,
                           uint32_t sb_org_y) {
    uint16_t tile_idx = context_ptr->tile_index;

    const BlockGeom *blk_geom = get_blk_geom_mds(blk_mds);

    uint32_t blk_org_x    = sb_org_x + blk_geom->origin_x;
    uint32_t blk_org_y    = sb_org_y + blk_geom->origin_y;
    uint32_t blk_org_x_uv = (blk_org_x >> 3 << 3) >> 1;
    uint32_t blk_org_y_uv = (blk_org_y >> 3 << 3) >> 1;
    uint32_t bwidth_uv    = blk_geom->bwidth_uv;
    uint32_t bheight_uv   = blk_geom->bheight_uv;

    copy_neigh_arr(pcs_ptr->md_intra_luma_mode_neighbor_array[src_idx][tile_idx],
                   pcs_ptr->md_intra_luma_mode_neighbor_array[dst_idx][tile_idx],
                   blk_org_x,
                   blk_org_y,
                   blk_geom->bwidth,
                   blk_geom->bheight,
                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
#if !CLN_MDC_CTX
    //neighbor_array_unit_reset(pcs_ptr->md_intra_chroma_mode_neighbor_array[depth]);
    copy_neigh_arr(pcs_ptr->md_intra_chroma_mode_neighbor_array[src_idx][tile_idx],
                   pcs_ptr->md_intra_chroma_mode_neighbor_array[dst_idx][tile_idx],
                   blk_org_x_uv,
                   blk_org_y_uv,
                   bwidth_uv,
                   bheight_uv,
                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
#endif
    //neighbor_array_unit_reset(pcs_ptr->md_skip_flag_neighbor_array[depth]);
    copy_neigh_arr(pcs_ptr->md_skip_flag_neighbor_array[src_idx][tile_idx],
                   pcs_ptr->md_skip_flag_neighbor_array[dst_idx][tile_idx],
                   blk_org_x,
                   blk_org_y,
                   blk_geom->bwidth,
                   blk_geom->bheight,
                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

    //neighbor_array_unit_reset(pcs_ptr->md_mode_type_neighbor_array[depth]);
    copy_neigh_arr(pcs_ptr->md_mode_type_neighbor_array[src_idx][tile_idx],
                   pcs_ptr->md_mode_type_neighbor_array[dst_idx][tile_idx],
                   blk_org_x,
                   blk_org_y,
                   blk_geom->bwidth,
                   blk_geom->bheight,
                   NEIGHBOR_ARRAY_UNIT_FULL_MASK);

    //neighbor_array_unit_reset(pcs_ptr->md_leaf_depth_neighbor_array[depth]);
    copy_neigh_arr(pcs_ptr->mdleaf_partition_neighbor_array[src_idx][tile_idx],
                   pcs_ptr->mdleaf_partition_neighbor_array[dst_idx][tile_idx],
                   blk_org_x,
                   blk_org_y,
                   blk_geom->bwidth,
                   blk_geom->bheight,
                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

    if (!context_ptr->hbd_mode_decision) {
        copy_neigh_arr(pcs_ptr->md_luma_recon_neighbor_array[src_idx][tile_idx],
                       pcs_ptr->md_luma_recon_neighbor_array[dst_idx][tile_idx],
                       blk_org_x,
                       blk_org_y,
                       blk_geom->bwidth,
                       blk_geom->bheight,
                       NEIGHBOR_ARRAY_UNIT_FULL_MASK);
        if (context_ptr->md_tx_size_search_mode) {
            copy_neigh_arr(pcs_ptr->md_tx_depth_1_luma_recon_neighbor_array[src_idx][tile_idx],
                           pcs_ptr->md_tx_depth_1_luma_recon_neighbor_array[dst_idx][tile_idx],
                           blk_org_x,
                           blk_org_y,
                           blk_geom->bwidth,
                           blk_geom->bheight,
                           NEIGHBOR_ARRAY_UNIT_FULL_MASK);
            copy_neigh_arr(pcs_ptr->md_tx_depth_2_luma_recon_neighbor_array[src_idx][tile_idx],
                           pcs_ptr->md_tx_depth_2_luma_recon_neighbor_array[dst_idx][tile_idx],
                           blk_org_x,
                           blk_org_y,
                           blk_geom->bwidth,
                           blk_geom->bheight,
                           NEIGHBOR_ARRAY_UNIT_FULL_MASK);
        }
        if (blk_geom->has_uv && context_ptr->chroma_level <= CHROMA_MODE_1) {
            copy_neigh_arr(pcs_ptr->md_cb_recon_neighbor_array[src_idx][tile_idx],
                           pcs_ptr->md_cb_recon_neighbor_array[dst_idx][tile_idx],
                           blk_org_x_uv,
                           blk_org_y_uv,
                           bwidth_uv,
                           bheight_uv,
                           NEIGHBOR_ARRAY_UNIT_FULL_MASK);

            copy_neigh_arr(pcs_ptr->md_cr_recon_neighbor_array[src_idx][tile_idx],
                           pcs_ptr->md_cr_recon_neighbor_array[dst_idx][tile_idx],
                           blk_org_x_uv,
                           blk_org_y_uv,
                           bwidth_uv,
                           bheight_uv,
                           NEIGHBOR_ARRAY_UNIT_FULL_MASK);
        }
    } else {
        copy_neigh_arr(pcs_ptr->md_luma_recon_neighbor_array16bit[src_idx][tile_idx],
                       pcs_ptr->md_luma_recon_neighbor_array16bit[dst_idx][tile_idx],
                       blk_org_x,
                       blk_org_y,
                       blk_geom->bwidth,
                       blk_geom->bheight,
                       NEIGHBOR_ARRAY_UNIT_FULL_MASK);
        if (context_ptr->md_tx_size_search_mode) {
            copy_neigh_arr(pcs_ptr->md_tx_depth_1_luma_recon_neighbor_array16bit[src_idx][tile_idx],
                           pcs_ptr->md_tx_depth_1_luma_recon_neighbor_array16bit[dst_idx][tile_idx],
                           blk_org_x,
                           blk_org_y,
                           blk_geom->bwidth,
                           blk_geom->bheight,
                           NEIGHBOR_ARRAY_UNIT_FULL_MASK);
            copy_neigh_arr(pcs_ptr->md_tx_depth_2_luma_recon_neighbor_array16bit[src_idx][tile_idx],
                           pcs_ptr->md_tx_depth_2_luma_recon_neighbor_array16bit[dst_idx][tile_idx],
                           blk_org_x,
                           blk_org_y,
                           blk_geom->bwidth,
                           blk_geom->bheight,
                           NEIGHBOR_ARRAY_UNIT_FULL_MASK);
        }

        if (blk_geom->has_uv && context_ptr->chroma_level <= CHROMA_MODE_1) {
            copy_neigh_arr(pcs_ptr->md_cb_recon_neighbor_array16bit[src_idx][tile_idx],
                           pcs_ptr->md_cb_recon_neighbor_array16bit[dst_idx][tile_idx],
                           blk_org_x_uv,
                           blk_org_y_uv,
                           bwidth_uv,
                           bheight_uv,
                           NEIGHBOR_ARRAY_UNIT_FULL_MASK);

            copy_neigh_arr(pcs_ptr->md_cr_recon_neighbor_array16bit[src_idx][tile_idx],
                           pcs_ptr->md_cr_recon_neighbor_array16bit[dst_idx][tile_idx],
                           blk_org_x_uv,
                           blk_org_y_uv,
                           bwidth_uv,
                           bheight_uv,
                           NEIGHBOR_ARRAY_UNIT_FULL_MASK);
        }
    }

    //neighbor_array_unit_reset(pcs_ptr->md_luma_dc_sign_level_coeff_neighbor_array[depth]);
    copy_neigh_arr(pcs_ptr->md_luma_dc_sign_level_coeff_neighbor_array[src_idx][tile_idx],
                   pcs_ptr->md_luma_dc_sign_level_coeff_neighbor_array[dst_idx][tile_idx],
                   blk_org_x,
                   blk_org_y,
                   blk_geom->bwidth,
                   blk_geom->bheight,
                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

    copy_neigh_arr(
        pcs_ptr->md_tx_depth_1_luma_dc_sign_level_coeff_neighbor_array[src_idx][tile_idx],
        pcs_ptr->md_tx_depth_1_luma_dc_sign_level_coeff_neighbor_array[dst_idx][tile_idx],
        blk_org_x,
        blk_org_y,
        blk_geom->bwidth,
        blk_geom->bheight,
        NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

    if (blk_geom->has_uv && context_ptr->chroma_level <= CHROMA_MODE_1) {
        copy_neigh_arr(pcs_ptr->md_cb_dc_sign_level_coeff_neighbor_array[src_idx][tile_idx],
                       pcs_ptr->md_cb_dc_sign_level_coeff_neighbor_array[dst_idx][tile_idx],
                       blk_org_x_uv,
                       blk_org_y_uv,
                       bwidth_uv,
                       bheight_uv,
                       NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
        //neighbor_array_unit_reset(pcs_ptr->md_cr_dc_sign_level_coeff_neighbor_array[depth]);

        copy_neigh_arr(pcs_ptr->md_cr_dc_sign_level_coeff_neighbor_array[src_idx][tile_idx],
                       pcs_ptr->md_cr_dc_sign_level_coeff_neighbor_array[dst_idx][tile_idx],
                       blk_org_x_uv,
                       blk_org_y_uv,
                       bwidth_uv,
                       bheight_uv,
                       NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
    }

    //neighbor_array_unit_reset(pcs_ptr->md_txfm_context_array[depth]);
    copy_neigh_arr(pcs_ptr->md_txfm_context_array[src_idx][tile_idx],
                   pcs_ptr->md_txfm_context_array[dst_idx][tile_idx],
                   blk_org_x,
                   blk_org_y,
                   blk_geom->bwidth,
                   blk_geom->bheight,
                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
#if !CLN_MDC_CTX
    //neighbor_array_unit_reset(pcs_ptr->md_inter_pred_dir_neighbor_array[depth]);
    copy_neigh_arr(pcs_ptr->md_inter_pred_dir_neighbor_array[src_idx][tile_idx],
                   pcs_ptr->md_inter_pred_dir_neighbor_array[dst_idx][tile_idx],
                   blk_org_x,
                   blk_org_y,
                   blk_geom->bwidth,
                   blk_geom->bheight,
                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
#endif
    //neighbor_array_unit_reset(pcs_ptr->md_ref_frame_type_neighbor_array[depth]);
    copy_neigh_arr(pcs_ptr->md_ref_frame_type_neighbor_array[src_idx][tile_idx],
                   pcs_ptr->md_ref_frame_type_neighbor_array[dst_idx][tile_idx],
                   blk_org_x,
                   blk_org_y,
                   blk_geom->bwidth,
                   blk_geom->bheight,
                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

    copy_neigh_arr_32(pcs_ptr->md_interpolation_type_neighbor_array[src_idx][tile_idx],
                      pcs_ptr->md_interpolation_type_neighbor_array[dst_idx][tile_idx],
                      blk_org_x,
                      blk_org_y,
                      blk_geom->bwidth,
                      blk_geom->bheight,
                      NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
}

void md_update_all_neighbour_arrays(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                                    uint32_t last_blk_index_mds, uint32_t sb_origin_x,
                                    uint32_t sb_origin_y) {
    context_ptr->blk_geom       = get_blk_geom_mds(last_blk_index_mds);
    context_ptr->blk_origin_x   = sb_origin_x + context_ptr->blk_geom->origin_x;
    context_ptr->blk_origin_y   = sb_origin_y + context_ptr->blk_geom->origin_y;
    context_ptr->round_origin_x = ((context_ptr->blk_origin_x >> 3) << 3);
    context_ptr->round_origin_y = ((context_ptr->blk_origin_y >> 3) << 3);

    context_ptr->blk_ptr   = &context_ptr->md_blk_arr_nsq[last_blk_index_mds];
    uint8_t avail_blk_flag = context_ptr->avail_blk_flag[last_blk_index_mds];
    if (avail_blk_flag) {
        mode_decision_update_neighbor_arrays(pcs_ptr, context_ptr, last_blk_index_mds);
        if (!context_ptr->shut_fast_rate || !context_ptr->shut_skip_ctx_dc_sign_update)
#if OPT_MI_UPDATE
            update_mi_map(context_ptr->blk_ptr,
                          context_ptr->blk_origin_x,
                          context_ptr->blk_origin_y,
                          context_ptr->blk_geom,
                          avail_blk_flag,
                          pcs_ptr);
#else
            update_mi_map(context_ptr,
                          context_ptr->blk_ptr,
                          context_ptr->blk_origin_x,
                          context_ptr->blk_origin_y,
                          context_ptr->blk_geom,
                          avail_blk_flag,
                          pcs_ptr);
#endif
    }
}

void md_update_all_neighbour_arrays_multiple(PictureControlSet *  pcs_ptr,
                                             ModeDecisionContext *context_ptr, uint32_t blk_mds,
                                             uint32_t sb_origin_x, uint32_t sb_origin_y) {
    context_ptr->blk_geom = get_blk_geom_mds(blk_mds);

    uint32_t blk_it;
    for (blk_it = 0; blk_it < context_ptr->blk_geom->totns; blk_it++) {
        md_update_all_neighbour_arrays(
            pcs_ptr, context_ptr, blk_mds + blk_it, sb_origin_x, sb_origin_y);
    }
}

#define TOTAL_SQ_BLOCK_COUNT 341
int sq_block_index[TOTAL_SQ_BLOCK_COUNT] = {
    0,    25,   50,   75,   80,   81,   82,   83,   84,   89,   90,   91,   92,   93,   98,   99,
    100,  101,  102,  107,  108,  109,  110,  111,  136,  141,  142,  143,  144,  145,  150,  151,
    152,  153,  154,  159,  160,  161,  162,  163,  168,  169,  170,  171,  172,  197,  202,  203,
    204,  205,  206,  211,  212,  213,  214,  215,  220,  221,  222,  223,  224,  229,  230,  231,
    232,  233,  258,  263,  264,  265,  266,  267,  272,  273,  274,  275,  276,  281,  282,  283,
    284,  285,  290,  291,  292,  293,  294,  319,  344,  349,  350,  351,  352,  353,  358,  359,
    360,  361,  362,  367,  368,  369,  370,  371,  376,  377,  378,  379,  380,  405,  410,  411,
    412,  413,  414,  419,  420,  421,  422,  423,  428,  429,  430,  431,  432,  437,  438,  439,
    440,  441,  466,  471,  472,  473,  474,  475,  480,  481,  482,  483,  484,  489,  490,  491,
    492,  493,  498,  499,  500,  501,  502,  527,  532,  533,  534,  535,  536,  541,  542,  543,
    544,  545,  550,  551,  552,  553,  554,  559,  560,  561,  562,  563,  588,  613,  618,  619,
    620,  621,  622,  627,  628,  629,  630,  631,  636,  637,  638,  639,  640,  645,  646,  647,
    648,  649,  674,  679,  680,  681,  682,  683,  688,  689,  690,  691,  692,  697,  698,  699,
    700,  701,  706,  707,  708,  709,  710,  735,  740,  741,  742,  743,  744,  749,  750,  751,
    752,  753,  758,  759,  760,  761,  762,  767,  768,  769,  770,  771,  796,  801,  802,  803,
    804,  805,  810,  811,  812,  813,  814,  819,  820,  821,  822,  823,  828,  829,  830,  831,
    832,  857,  882,  887,  888,  889,  890,  891,  896,  897,  898,  899,  900,  905,  906,  907,
    908,  909,  914,  915,  916,  917,  918,  943,  948,  949,  950,  951,  952,  957,  958,  959,
    960,  961,  966,  967,  968,  969,  970,  975,  976,  977,  978,  979,  1004, 1009, 1010, 1011,
    1012, 1013, 1018, 1019, 1020, 1021, 1022, 1027, 1028, 1029, 1030, 1031, 1036, 1037, 1038, 1039,
    1040, 1065, 1070, 1071, 1072, 1073, 1074, 1079, 1080, 1081, 1082, 1083, 1088, 1089, 1090, 1091,
    1092, 1097, 1098, 1099, 1100};
void av1_perform_inverse_transform_recon_luma(ModeDecisionContext *        context_ptr,
                                              ModeDecisionCandidateBuffer *candidate_buffer) {
    uint32_t tu_total_count;
    uint32_t txb_itr;

    uint8_t tx_depth       = candidate_buffer->candidate_ptr->tx_depth;
    tu_total_count         = context_ptr->blk_geom->txb_count[tx_depth];
    txb_itr                = 0;
    uint32_t txb_1d_offset = 0;
    int32_t  is_inter      = (candidate_buffer->candidate_ptr->type == INTER_MODE ||
                        candidate_buffer->candidate_ptr->use_intrabc)
              ? EB_TRUE
              : EB_FALSE;
    do {
        uint32_t txb_origin_x     = context_ptr->blk_geom->tx_org_x[is_inter][tx_depth][txb_itr];
        uint32_t txb_origin_y     = context_ptr->blk_geom->tx_org_y[is_inter][tx_depth][txb_itr];
        uint32_t txb_width        = context_ptr->blk_geom->tx_width[tx_depth][txb_itr];
        uint32_t txb_height       = context_ptr->blk_geom->tx_height[tx_depth][txb_itr];
        uint32_t txb_origin_index = txb_origin_x +
            txb_origin_y * candidate_buffer->prediction_ptr->stride_y;
        uint32_t rec_luma_offset = txb_origin_x +
            txb_origin_y * candidate_buffer->recon_ptr->stride_y;
        uint32_t y_has_coeff = (candidate_buffer->candidate_ptr->y_has_coeff & (1 << txb_itr)) > 0;

        if (y_has_coeff)
            inv_transform_recon_wrapper(candidate_buffer->prediction_ptr->buffer_y,
                                        txb_origin_index,
                                        candidate_buffer->prediction_ptr->stride_y,
                                        context_ptr->hbd_mode_decision
                                            ? (uint8_t *)context_ptr->cfl_temp_luma_recon16bit
                                            : context_ptr->cfl_temp_luma_recon,
                                        rec_luma_offset,
                                        candidate_buffer->recon_ptr->stride_y,
                                        (int32_t *)candidate_buffer->recon_coeff_ptr->buffer_y,
                                        txb_1d_offset,
                                        context_ptr->hbd_mode_decision,
                                        context_ptr->blk_geom->txsize[tx_depth][txb_itr],
                                        candidate_buffer->candidate_ptr->transform_type[txb_itr],
                                        PLANE_TYPE_Y,
                                        (uint32_t)candidate_buffer->candidate_ptr->eob[0][txb_itr]);
        else {
            if (context_ptr->hbd_mode_decision) {
                pic_copy_kernel_16bit(
                    ((uint16_t *)candidate_buffer->prediction_ptr->buffer_y) + txb_origin_index,
                    candidate_buffer->prediction_ptr->stride_y,
                    context_ptr->cfl_temp_luma_recon16bit + rec_luma_offset,
                    candidate_buffer->recon_ptr->stride_y,
                    txb_width,
                    txb_height);
            } else {
                pic_copy_kernel_8bit(
                    &(candidate_buffer->prediction_ptr->buffer_y[txb_origin_index]),
                    candidate_buffer->prediction_ptr->stride_y,
                    &(context_ptr->cfl_temp_luma_recon[rec_luma_offset]),
                    candidate_buffer->recon_ptr->stride_y,
                    txb_width,
                    txb_height);
            }
        }
        txb_1d_offset += context_ptr->blk_geom->tx_width[tx_depth][txb_itr] *
            context_ptr->blk_geom->tx_height[tx_depth][txb_itr];
        ++txb_itr;
    } while (txb_itr < tu_total_count);
}
void av1_perform_inverse_transform_recon(ModeDecisionContext *        context_ptr,
                                         ModeDecisionCandidateBuffer *candidate_buffer,
                                         const BlockGeom *            blk_geom) {
    uint32_t tu_total_count;
    uint32_t txb_index;
    uint32_t txb_itr;

    UNUSED(blk_geom);

    const uint8_t tx_depth = candidate_buffer->candidate_ptr->tx_depth;
    tu_total_count         = context_ptr->blk_geom->txb_count[tx_depth];
    txb_index              = 0;
    txb_itr                = 0;
    uint32_t txb_1d_offset = 0, txb_1d_offset_uv = 0;
    int32_t  is_inter = candidate_buffer->candidate_ptr->type == INTER_MODE ||
        candidate_buffer->candidate_ptr->use_intrabc;

    do {
        uint32_t txb_origin_x    = context_ptr->blk_geom->tx_org_x[is_inter][tx_depth][txb_itr];
        uint32_t txb_origin_y    = context_ptr->blk_geom->tx_org_y[is_inter][tx_depth][txb_itr];
        uint32_t txb_width       = context_ptr->blk_geom->tx_width[tx_depth][txb_itr];
        uint32_t txb_height      = context_ptr->blk_geom->tx_height[tx_depth][txb_itr];
        uint32_t rec_luma_offset = txb_origin_x +
            txb_origin_y * candidate_buffer->recon_ptr->stride_y;
        uint32_t rec_cb_offset    = ((((txb_origin_x >> 3) << 3) +
                                   ((txb_origin_y >> 3) << 3) *
                                       candidate_buffer->recon_ptr->stride_cb) >>
                                  1);
        uint32_t rec_cr_offset    = ((((txb_origin_x >> 3) << 3) +
                                   ((txb_origin_y >> 3) << 3) *
                                       candidate_buffer->recon_ptr->stride_cr) >>
                                  1);
        uint32_t txb_origin_index = txb_origin_x +
            txb_origin_y * candidate_buffer->prediction_ptr->stride_y;
        if (context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].y_has_coeff[txb_itr])
            inv_transform_recon_wrapper(candidate_buffer->prediction_ptr->buffer_y,
                                        txb_origin_index,
                                        candidate_buffer->prediction_ptr->stride_y,
                                        candidate_buffer->recon_ptr->buffer_y,
                                        rec_luma_offset,
                                        candidate_buffer->recon_ptr->stride_y,
                                        (int32_t *)candidate_buffer->recon_coeff_ptr->buffer_y,
                                        txb_1d_offset,
                                        context_ptr->hbd_mode_decision,
                                        context_ptr->blk_geom->txsize[tx_depth][txb_itr],
                                        candidate_buffer->candidate_ptr->transform_type[txb_itr],
                                        PLANE_TYPE_Y,
                                        (uint32_t)candidate_buffer->candidate_ptr->eob[0][txb_itr]);
        else
            svt_av1_picture_copy(candidate_buffer->prediction_ptr,
                                 txb_origin_index,
                                 0, //txb_chroma_origin_index,
                                 candidate_buffer->recon_ptr,
                                 rec_luma_offset,
                                 0, //txb_chroma_origin_index,
                                 txb_width,
                                 txb_height,
                                 0, //chromaTuSize,
                                 0, //chromaTuSize,
                                 PICTURE_BUFFER_DESC_Y_FLAG,
                                 context_ptr->hbd_mode_decision);

        //CHROMA
        if (tx_depth == 0 || txb_itr == 0) {
            if (context_ptr->chroma_level <= CHROMA_MODE_1) {
                uint32_t chroma_txb_width =
                    tx_size_wide[context_ptr->blk_geom->txsize_uv[tx_depth][txb_itr]];
                uint32_t chroma_txb_height =
                    tx_size_high[context_ptr->blk_geom->txsize_uv[tx_depth][txb_itr]];
                uint32_t cb_tu_chroma_origin_index =
                    ((((txb_origin_x >> 3) << 3) +
                      ((txb_origin_y >> 3) << 3) * candidate_buffer->recon_coeff_ptr->stride_cb) >>
                     1);
                uint32_t cr_tu_chroma_origin_index =
                    ((((txb_origin_x >> 3) << 3) +
                      ((txb_origin_y >> 3) << 3) * candidate_buffer->recon_coeff_ptr->stride_cr) >>
                     1);

                if (context_ptr->blk_geom->has_uv &&
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .u_has_coeff[txb_index])
                    inv_transform_recon_wrapper(
                        candidate_buffer->prediction_ptr->buffer_cb,
                        cb_tu_chroma_origin_index,
                        candidate_buffer->prediction_ptr->stride_cb,
                        candidate_buffer->recon_ptr->buffer_cb,
                        rec_cb_offset,
                        candidate_buffer->recon_ptr->stride_cb,
                        (int32_t *)candidate_buffer->recon_coeff_ptr->buffer_cb,
                        txb_1d_offset_uv,
                        context_ptr->hbd_mode_decision,
                        context_ptr->blk_geom->txsize_uv[tx_depth][txb_itr],
                        candidate_buffer->candidate_ptr->transform_type_uv,
                        PLANE_TYPE_UV,
                        (uint32_t)candidate_buffer->candidate_ptr->eob[1][txb_itr]);
                else
                    svt_av1_picture_copy(candidate_buffer->prediction_ptr,
                                         0,
                                         cb_tu_chroma_origin_index,
                                         candidate_buffer->recon_ptr,
                                         0,
                                         rec_cb_offset,
                                         0,
                                         0,
                                         chroma_txb_width,
                                         chroma_txb_height,
                                         PICTURE_BUFFER_DESC_Cb_FLAG,
                                         context_ptr->hbd_mode_decision);

                if (context_ptr->blk_geom->has_uv &&
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .v_has_coeff[txb_index])
                    inv_transform_recon_wrapper(
                        candidate_buffer->prediction_ptr->buffer_cr,
                        cr_tu_chroma_origin_index,
                        candidate_buffer->prediction_ptr->stride_cr,
                        candidate_buffer->recon_ptr->buffer_cr,
                        rec_cr_offset,
                        candidate_buffer->recon_ptr->stride_cr,
                        (int32_t *)candidate_buffer->recon_coeff_ptr->buffer_cr,
                        txb_1d_offset_uv,
                        context_ptr->hbd_mode_decision,
                        context_ptr->blk_geom->txsize_uv[tx_depth][txb_itr],
                        candidate_buffer->candidate_ptr->transform_type_uv,
                        PLANE_TYPE_UV,
                        (uint32_t)candidate_buffer->candidate_ptr->eob[2][txb_itr]);
                else
                    svt_av1_picture_copy(candidate_buffer->prediction_ptr,
                                         0,
                                         cr_tu_chroma_origin_index,
                                         candidate_buffer->recon_ptr,
                                         0,
                                         rec_cr_offset,
                                         0,
                                         0,
                                         chroma_txb_width,
                                         chroma_txb_height,
                                         PICTURE_BUFFER_DESC_Cr_FLAG,
                                         context_ptr->hbd_mode_decision);

                if (context_ptr->blk_geom->has_uv)
                    txb_1d_offset_uv += context_ptr->blk_geom->tx_width_uv[tx_depth][txb_itr] *
                        context_ptr->blk_geom->tx_height_uv[tx_depth][txb_itr];
            }
        }
        txb_1d_offset += context_ptr->blk_geom->tx_width[tx_depth][txb_itr] *
            context_ptr->blk_geom->tx_height[tx_depth][txb_itr];
        ++txb_index;
        ++txb_itr;
    } while (txb_itr < tu_total_count);
}

/*******************************************
* Coding Loop - Fast Loop Initialization
*******************************************/
#if OPT_INIT_XD
void product_coding_loop_init_fast_loop(PictureControlSet *pcs_ptr,
                                        ModeDecisionContext *context_ptr,
                                        NeighborArrayUnit *  skip_flag_neighbor_array,
                                        NeighborArrayUnit *  mode_type_neighbor_array,
                                        NeighborArrayUnit *  leaf_partition_neighbor_array) {
#else
void product_coding_loop_init_fast_loop(ModeDecisionContext *context_ptr,
#if !CLN_MDC_CTX
                                        NeighborArrayUnit *inter_pred_dir_neighbor_array,
                                        NeighborArrayUnit *ref_frame_type_neighbor_array,
#endif
#if !CLN_MDC_CTX
                                        NeighborArrayUnit *intra_luma_mode_neighbor_array,
#endif
                                        NeighborArrayUnit *skip_flag_neighbor_array,
                                        NeighborArrayUnit *mode_type_neighbor_array,
                                        NeighborArrayUnit *leaf_partition_neighbor_array) {
#endif
    context_ptr->tx_depth = context_ptr->blk_ptr->tx_depth = 0;
    // Generate Split, Skip and intra mode contexts for the rate estimation
#if OPT_INIT_XD
    coding_loop_context_generation(pcs_ptr,
                                   context_ptr,
#else
    coding_loop_context_generation(context_ptr,
#endif
                                   context_ptr->blk_ptr,
                                   context_ptr->blk_origin_x,
                                   context_ptr->blk_origin_y,
#if !CLN_MDC_CTX
                                   BLOCK_SIZE_64,
#endif
#if !CLN_MDC_CTX
                                   inter_pred_dir_neighbor_array,
                                   ref_frame_type_neighbor_array,
#endif
#if !CLN_MDC_CTX
                                   intra_luma_mode_neighbor_array,
#endif
                                   skip_flag_neighbor_array,
                                   mode_type_neighbor_array,
                                   leaf_partition_neighbor_array);

    return;
}

void fast_loop_core(ModeDecisionCandidateBuffer *candidate_buffer, PictureControlSet *pcs_ptr,
                    ModeDecisionContext *context_ptr, EbPictureBufferDesc *input_picture_ptr,
                    uint32_t input_origin_index, uint32_t input_cb_origin_in_index,
                    uint32_t input_cr_origin_in_index, BlkStruct *blk_ptr, uint32_t cu_origin_index,
                    uint32_t cu_chroma_origin_index, EbBool use_ssd) {
    uint64_t luma_fast_distortion;
    uint64_t chroma_fast_distortion;
    uint32_t full_lambda = context_ptr->hbd_mode_decision
        ? context_ptr->full_lambda_md[EB_10_BIT_MD]
        : context_ptr->full_lambda_md[EB_8_BIT_MD];
    uint32_t fast_lambda = context_ptr->hbd_mode_decision
        ? context_ptr->fast_lambda_md[EB_10_BIT_MD]
        : context_ptr->fast_lambda_md[EB_8_BIT_MD];

    ModeDecisionCandidate *candidate_ptr  = candidate_buffer->candidate_ptr;
    EbPictureBufferDesc *  prediction_ptr = candidate_buffer->prediction_ptr;
    context_ptr->pu_itr                   = 0;
    // Prediction
    // Set default interp_filters
    candidate_buffer->candidate_ptr->interp_filters = 0;
    context_ptr->uv_intra_comp_only                 = EB_FALSE;
    svt_product_prediction_fun_table[candidate_buffer->candidate_ptr->use_intrabc
                                         ? INTER_MODE
                                         : candidate_ptr->type](
        context_ptr->hbd_mode_decision, context_ptr, pcs_ptr, candidate_buffer);

    // Distortion
    // Y
    if (use_ssd) {
        EbSpatialFullDistType spatial_full_dist_type_fun = context_ptr->hbd_mode_decision
            ? svt_full_distortion_kernel16_bits
            : svt_spatial_full_distortion_kernel;

        candidate_buffer->candidate_ptr->luma_fast_distortion = (uint32_t)(
            luma_fast_distortion = spatial_full_dist_type_fun(input_picture_ptr->buffer_y,
                                                              input_origin_index,
                                                              input_picture_ptr->stride_y,
                                                              prediction_ptr->buffer_y,
                                                              (int32_t)cu_origin_index,
                                                              prediction_ptr->stride_y,
                                                              context_ptr->blk_geom->bwidth,
                                                              context_ptr->blk_geom->bheight));
#if FTR_USE_VAR_IN_FAST_LOOP
    } else if (context_ptr->use_var_in_mds0) {
        if (!context_ptr->hbd_mode_decision) {
            const AomVarianceFnPtr *fn_ptr = &mefn_ptr[context_ptr->blk_geom->bsize];
            unsigned int            sse;
            uint8_t *               pred_y = prediction_ptr->buffer_y + cu_origin_index;
            uint8_t *               src_y  = input_picture_ptr->buffer_y + input_origin_index;
            luma_fast_distortion           = fn_ptr->vf(pred_y,
                                              prediction_ptr->stride_y,
                                              src_y,
                                              input_picture_ptr->stride_y,
                                              &sse) >>
                2;
        } else {
            const AomVarianceFnPtr *fn_ptr = &mefn_ptr[context_ptr->blk_geom->bsize];
            unsigned int            sse;
            uint16_t *pred_y = ((uint16_t *)prediction_ptr->buffer_y) + cu_origin_index;
            uint16_t *src_y  = ((uint16_t *)input_picture_ptr->buffer_y) + input_origin_index;
#if FTR_USE_VAR_IN_FAST_LOOP10BIT
            luma_fast_distortion = fn_ptr->vf_hbd_10(CONVERT_TO_BYTEPTR(pred_y),
                                                     prediction_ptr->stride_y,
                                                     CONVERT_TO_BYTEPTR(src_y),
                                                     input_picture_ptr->stride_y,
                                                     &sse) >>
                1;
#else
            luma_fast_distortion = fn_ptr->vf_hbd_10(CONVERT_TO_BYTEPTR(pred_y),
                                                     prediction_ptr->stride_y,
                                                     CONVERT_TO_BYTEPTR(src_y),
                                                     input_picture_ptr->stride_y,
                                                     &sse) >>
                2;
#endif
        }
#endif
    } else {
        assert((context_ptr->blk_geom->bwidth >> 3) < 17);
        if (!context_ptr->hbd_mode_decision) {
            candidate_buffer->candidate_ptr->luma_fast_distortion = (uint32_t)(
                luma_fast_distortion = svt_nxm_sad_kernel_sub_sampled(
                    input_picture_ptr->buffer_y + input_origin_index,
                    input_picture_ptr->stride_y,
                    prediction_ptr->buffer_y + cu_origin_index,
                    prediction_ptr->stride_y,
                    context_ptr->blk_geom->bheight,
                    context_ptr->blk_geom->bwidth));
        } else {
            candidate_buffer->candidate_ptr->luma_fast_distortion = (uint32_t)(
                luma_fast_distortion = sad_16b_kernel(
                    ((uint16_t *)input_picture_ptr->buffer_y) + input_origin_index,
                    input_picture_ptr->stride_y,
                    ((uint16_t *)prediction_ptr->buffer_y) + cu_origin_index,
                    prediction_ptr->stride_y,
                    context_ptr->blk_geom->bheight,
                    context_ptr->blk_geom->bwidth));
        }
    }

    if (context_ptr->blk_geom->has_uv && context_ptr->chroma_level <= CHROMA_MODE_1 &&
        context_ptr->md_staging_skip_chroma_pred == EB_FALSE) {
        if (use_ssd) {
            EbSpatialFullDistType spatial_full_dist_type_fun = context_ptr->hbd_mode_decision
                ? svt_full_distortion_kernel16_bits
                : svt_spatial_full_distortion_kernel;

            chroma_fast_distortion = spatial_full_dist_type_fun(
                input_picture_ptr->buffer_cb,
                input_cb_origin_in_index,
                input_picture_ptr->stride_cb,
                candidate_buffer->prediction_ptr->buffer_cb,
                (int32_t)cu_chroma_origin_index,
                prediction_ptr->stride_cb,
                context_ptr->blk_geom->bwidth_uv,
                context_ptr->blk_geom->bheight_uv);

            chroma_fast_distortion += spatial_full_dist_type_fun(
                input_picture_ptr->buffer_cr,
                input_cr_origin_in_index,
                input_picture_ptr->stride_cb,
                candidate_buffer->prediction_ptr->buffer_cr,
                (int32_t)cu_chroma_origin_index,
                prediction_ptr->stride_cr,
                context_ptr->blk_geom->bwidth_uv,
                context_ptr->blk_geom->bheight_uv);
        } else {
            assert((context_ptr->blk_geom->bwidth_uv >> 3) < 17);

            if (!context_ptr->hbd_mode_decision) {
                chroma_fast_distortion = svt_nxm_sad_kernel_sub_sampled(
                    input_picture_ptr->buffer_cb + input_cb_origin_in_index,
                    input_picture_ptr->stride_cb,
                    candidate_buffer->prediction_ptr->buffer_cb + cu_chroma_origin_index,
                    prediction_ptr->stride_cb,
                    context_ptr->blk_geom->bheight_uv,
                    context_ptr->blk_geom->bwidth_uv);

                chroma_fast_distortion += svt_nxm_sad_kernel_sub_sampled(
                    input_picture_ptr->buffer_cr + input_cr_origin_in_index,
                    input_picture_ptr->stride_cr,
                    candidate_buffer->prediction_ptr->buffer_cr + cu_chroma_origin_index,
                    prediction_ptr->stride_cr,
                    context_ptr->blk_geom->bheight_uv,
                    context_ptr->blk_geom->bwidth_uv);
            } else {
                chroma_fast_distortion = sad_16b_kernel(
                    ((uint16_t *)input_picture_ptr->buffer_cb) + input_cb_origin_in_index,
                    input_picture_ptr->stride_cb,
                    ((uint16_t *)candidate_buffer->prediction_ptr->buffer_cb) +
                        cu_chroma_origin_index,
                    prediction_ptr->stride_cb,
                    context_ptr->blk_geom->bheight_uv,
                    context_ptr->blk_geom->bwidth_uv);

                chroma_fast_distortion += sad_16b_kernel(
                    ((uint16_t *)input_picture_ptr->buffer_cr) + input_cr_origin_in_index,
                    input_picture_ptr->stride_cr,
                    ((uint16_t *)candidate_buffer->prediction_ptr->buffer_cr) +
                        cu_chroma_origin_index,
                    prediction_ptr->stride_cr,
                    context_ptr->blk_geom->bheight_uv,
                    context_ptr->blk_geom->bwidth_uv);
            }
        }
    } else
        chroma_fast_distortion = 0;
    if (context_ptr->early_cand_elimination) {
        const uint64_t distortion_cost = RDCOST(
            use_ssd ? full_lambda : fast_lambda, 0, luma_fast_distortion + chroma_fast_distortion);
        if ((distortion_cost > context_ptr->mds0_best_cost) &&
            (context_ptr->mds0_best_class == CAND_CLASS_2)) {
            *(candidate_buffer->fast_cost_ptr) = MAX_MODE_COST;
            return;
        }
    }
    // Fast Cost
    if (context_ptr->shut_fast_rate) {
        *(candidate_buffer->fast_cost_ptr) = luma_fast_distortion + chroma_fast_distortion;
        candidate_ptr->fast_luma_rate      = 0;
        candidate_ptr->fast_chroma_rate    = 0;
    } else {
        *(candidate_buffer->fast_cost_ptr) = av1_product_fast_cost_func_table[candidate_ptr->type](
#if CLN_FAST_COST
            context_ptr,
#endif
            blk_ptr,
            candidate_buffer->candidate_ptr,
            NOT_USED_VALUE,
            luma_fast_distortion,
            chroma_fast_distortion,
            use_ssd ? full_lambda : fast_lambda,
            pcs_ptr,
            &(context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                  .ed_ref_mv_stack[candidate_ptr->ref_frame_type][0]),
            context_ptr->blk_geom,
            context_ptr->blk_origin_y >> MI_SIZE_LOG2,
            context_ptr->blk_origin_x >> MI_SIZE_LOG2,
            context_ptr->inter_intra_comp_ctrls.enabled,
#if !CLN_FAST_COST
            1,
#endif
            context_ptr->intra_luma_left_mode,
            context_ptr->intra_luma_top_mode);
    }
    // Init full cost in case we by pass stage1/stage2
    if (context_ptr->md_staging_mode == MD_STAGING_MODE_0)
        *(candidate_buffer->full_cost_ptr) = *(candidate_buffer->fast_cost_ptr);
}
#if !OPT_INIT
#if FTR_NEW_REF_PRUNING_CTRLS
void set_dist_based_ref_pruning_controls(ModeDecisionContext *mdctxt,
                                         uint8_t              dist_based_ref_pruning_level) {
    RefPruningControls *ref_pruning_ctrls = &mdctxt->ref_pruning_ctrls;

    switch (dist_based_ref_pruning_level) {
    case 0: ref_pruning_ctrls->enabled = 0; break;
    case 1:
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP]         = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP]       = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP]        = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP]     = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP]       = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP]   = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST]           = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF]           = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE]          = (uint32_t)~0;
        ref_pruning_ctrls->ref_idx_2_offset                     = 0;
        ref_pruning_ctrls->ref_idx_3_offset                     = 0;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]         = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]     = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]   = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]          = 1;

        break;
#if TUNE_NEW_PRESETS_MR_M8
    case 2:
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP]         = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP]       = 0;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP]        = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP]     = 90;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP]       = 90;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP]   = 60;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST]           = 60;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF]           = 60;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE]          = 60;
        ref_pruning_ctrls->ref_idx_2_offset                     = 10;
        ref_pruning_ctrls->ref_idx_3_offset                     = 20;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]         = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]     = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]   = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]          = 1;

        break;
    case 3: // M1
#else
    case 2:
#endif
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP]         = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP]       = 0;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP]        = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP]     = 60;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP]       = 60;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP]   = 30;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST]           = 30;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF]           = 30;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE]          = 30;
        ref_pruning_ctrls->ref_idx_2_offset                     = 10;
        ref_pruning_ctrls->ref_idx_3_offset                     = 20;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]         = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]     = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]   = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]          = 1;

        break;
#if TUNE_NEW_PRESETS_MR_M8
    case 4:
#else
    case 3:
#endif
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP]         = 60;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP]       = 0;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP]        = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP]     = 60;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP]       = 60;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP]          = 30;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP]   = 10;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST]           = 10;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF]           = 10;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE]          = 10;
        ref_pruning_ctrls->ref_idx_2_offset                     = 10;
        ref_pruning_ctrls->ref_idx_3_offset                     = 20;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]         = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]     = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]   = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]          = 1;

        break;
#if TUNE_NEW_PRESETS_MR_M8
    case 5:
#else
    case 4:
#endif
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP]         = 30;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP]       = 0;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP]        = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP]     = 30;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP]       = 10;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP]          = 10;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP]   = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST]           = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF]           = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE]          = 0;
        ref_pruning_ctrls->ref_idx_2_offset                     = 10;
        ref_pruning_ctrls->ref_idx_3_offset                     = 20;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]         = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]     = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]   = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]          = 1;
        break;
#if TUNE_NEW_PRESETS_MR_M8
    case 6: // M2-M5
#else
    case 5:
#endif
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP]         = 0;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP]       = 0;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP]        = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP]     = 0;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP]       = 0;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP]          = 0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP]          = 0;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP]   = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST]           = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF]           = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE]          = 0;
        ref_pruning_ctrls->ref_idx_2_offset                     = 0;
        ref_pruning_ctrls->ref_idx_3_offset                     = 0;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]         = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]     = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]   = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]          = 1;
        break;

    default: assert(0); break;
    }
}
#else
void set_dist_based_ref_pruning_controls(ModeDecisionContext *mdctxt,
                                         uint8_t dist_based_ref_pruning_level) {
    RefPruningControls *ref_pruning_ctrls = &mdctxt->ref_pruning_ctrls;

    switch (dist_based_ref_pruning_level) {
    case 0: ref_pruning_ctrls->enabled = 0; break;
    case 1:
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->best_refs[PA_ME_GROUP] = 7;
        ref_pruning_ctrls->best_refs[UNI_3x3_GROUP] = 7;
        ref_pruning_ctrls->best_refs[BI_3x3_GROUP] = 2;
        ref_pruning_ctrls->best_refs[NRST_NEW_NEAR_GROUP] = 0;
        ref_pruning_ctrls->best_refs[WARP_GROUP] = 7;
        ref_pruning_ctrls->best_refs[NRST_NEAR_GROUP] = 7;
        ref_pruning_ctrls->best_refs[PRED_ME_GROUP] = 7;
        ref_pruning_ctrls->best_refs[GLOBAL_GROUP] = 7;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP] = 1;
        break;
    case 2:
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->best_refs[PA_ME_GROUP] = 7;
        ref_pruning_ctrls->best_refs[UNI_3x3_GROUP] = 7;
        ref_pruning_ctrls->best_refs[BI_3x3_GROUP] = 2;
        ref_pruning_ctrls->best_refs[NRST_NEW_NEAR_GROUP] = 0;
        ref_pruning_ctrls->best_refs[WARP_GROUP] = 7;
        ref_pruning_ctrls->best_refs[NRST_NEAR_GROUP] = 6;
        ref_pruning_ctrls->best_refs[PRED_ME_GROUP] = 4;
        ref_pruning_ctrls->best_refs[GLOBAL_GROUP] = 7;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP] = 1;
        break;
    case 3:
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->best_refs[PA_ME_GROUP] = 7;
        ref_pruning_ctrls->best_refs[UNI_3x3_GROUP] = 7;
        ref_pruning_ctrls->best_refs[BI_3x3_GROUP] = 2;
        ref_pruning_ctrls->best_refs[NRST_NEW_NEAR_GROUP] = 0;
        ref_pruning_ctrls->best_refs[WARP_GROUP] = 7;
        ref_pruning_ctrls->best_refs[NRST_NEAR_GROUP] = 4;
        ref_pruning_ctrls->best_refs[PRED_ME_GROUP] = 4;
        ref_pruning_ctrls->best_refs[GLOBAL_GROUP] = 7;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP] = 1;
        break;
    case 4:
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->best_refs[PA_ME_GROUP] = 7;
        ref_pruning_ctrls->best_refs[UNI_3x3_GROUP] = 7;
        ref_pruning_ctrls->best_refs[BI_3x3_GROUP] = 2;
        ref_pruning_ctrls->best_refs[NRST_NEW_NEAR_GROUP] = 0;
        ref_pruning_ctrls->best_refs[WARP_GROUP] = 7;
        ref_pruning_ctrls->best_refs[NRST_NEAR_GROUP] = 4;
        ref_pruning_ctrls->best_refs[PRED_ME_GROUP] = 2;
        ref_pruning_ctrls->best_refs[GLOBAL_GROUP] = 7;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP] = 1;
        break;
    case 5:
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->best_refs[PA_ME_GROUP] = 1;
        ref_pruning_ctrls->best_refs[UNI_3x3_GROUP] = 1;
        ref_pruning_ctrls->best_refs[BI_3x3_GROUP] = 1;
        ref_pruning_ctrls->best_refs[NRST_NEW_NEAR_GROUP] = 0;
        ref_pruning_ctrls->best_refs[WARP_GROUP] = 1;
        ref_pruning_ctrls->best_refs[NRST_NEAR_GROUP] = 1;
        ref_pruning_ctrls->best_refs[PRED_ME_GROUP] = 1;
        ref_pruning_ctrls->best_refs[GLOBAL_GROUP] = 1;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP] = 1;
        break;
    case 6:
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->best_refs[PA_ME_GROUP] = 0;
        ref_pruning_ctrls->best_refs[UNI_3x3_GROUP] = 0;
        ref_pruning_ctrls->best_refs[BI_3x3_GROUP] = 0;
        ref_pruning_ctrls->best_refs[NRST_NEW_NEAR_GROUP] = 0;
        ref_pruning_ctrls->best_refs[WARP_GROUP] = 0;
        ref_pruning_ctrls->best_refs[NRST_NEAR_GROUP] = 0;
        ref_pruning_ctrls->best_refs[PRED_ME_GROUP] = 0;
        ref_pruning_ctrls->best_refs[GLOBAL_GROUP] = 0;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP] = 1;
        break;
    default: assert(0); break;
    }
}
#endif
#endif
void set_compound_to_inject(ModeDecisionContext *context_ptr, EbBool *comp_inj_table, EbBool avg,
                            EbBool dist, EbBool diff, EbBool wdg);

void set_inter_comp_controls(ModeDecisionContext *ctx, uint8_t inter_comp_mode) {
    InterCompCtrls *inter_comp_ctrls = &ctx->inter_comp_ctrls;

    switch (inter_comp_mode) {
    case 0: //OFF
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_comp_types,
                               1 /*AVG*/,
                               0 /*DIST*/,
                               0 /*DIFF*/,
                               0 /*WEDGE*/);
#if !FTR_NEW_REF_PRUNING_CTRLS
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_dist1_comp_types,
                               1 /*AVG*/,
                               0 /*DIST*/,
                               0 /*DIFF*/,
                               0 /*WEDGE*/);
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_dist2_comp_types,
                               1 /*AVG*/,
                               0 /*DIST*/,
                               0 /*DIFF*/,
                               0 /*WEDGE*/);
#endif
        break;
    case 1: //FULL
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               1 /*DIFF*/,
                               1 /*WEDGE*/);
#if FTR_UPGRADE_COMP_LEVELS
        inter_comp_ctrls->do_nearest_nearest  = 1;
        inter_comp_ctrls->do_near_near        = 1;
        inter_comp_ctrls->do_me               = 1;
        inter_comp_ctrls->do_pme              = 1;
        inter_comp_ctrls->do_nearest_near_new = 1;
        inter_comp_ctrls->do_3x3_bi           = 1;

        inter_comp_ctrls->use_rate            = 1;
        inter_comp_ctrls->pred0_to_pred1_mult = 0;

        break;
    case 2:
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               1 /*DIFF*/,
                               1 /*WEDGE*/);

        inter_comp_ctrls->do_nearest_nearest  = 1;
        inter_comp_ctrls->do_near_near        = 1;
        inter_comp_ctrls->do_me               = 1;
        inter_comp_ctrls->do_pme              = 1;
        inter_comp_ctrls->do_nearest_near_new = 1;
        inter_comp_ctrls->do_3x3_bi           = 0;

        inter_comp_ctrls->use_rate            = 0;
        inter_comp_ctrls->pred0_to_pred1_mult = 0;

        break;

    case 3:
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               1 /*DIFF*/,
                               1 /*WEDGE*/);

        inter_comp_ctrls->do_nearest_nearest  = 1;
        inter_comp_ctrls->do_near_near        = 1;
        inter_comp_ctrls->do_me               = 1;
        inter_comp_ctrls->do_pme              = 1;
        inter_comp_ctrls->do_nearest_near_new = 0;
        inter_comp_ctrls->do_3x3_bi           = 0;

        inter_comp_ctrls->use_rate            = 0;
        inter_comp_ctrls->pred0_to_pred1_mult = 1;

        break;
    case 4:
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               1 /*DIFF*/,
                               1 /*WEDGE*/);

        inter_comp_ctrls->do_nearest_nearest  = 1;
        inter_comp_ctrls->do_near_near        = 1;
        inter_comp_ctrls->do_me               = 0;
        inter_comp_ctrls->do_pme              = 0;
        inter_comp_ctrls->do_nearest_near_new = 0;
        inter_comp_ctrls->do_3x3_bi           = 0;

        inter_comp_ctrls->use_rate            = 0;
        inter_comp_ctrls->pred0_to_pred1_mult = 2;

        break;
    case 5:
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               1 /*DIFF*/,
                               1 /*WEDGE*/);

        inter_comp_ctrls->do_nearest_nearest  = 1;
        inter_comp_ctrls->do_near_near        = 0;
        inter_comp_ctrls->do_me               = 0;
        inter_comp_ctrls->do_pme              = 0;
        inter_comp_ctrls->do_nearest_near_new = 0;
        inter_comp_ctrls->do_3x3_bi           = 0;

        inter_comp_ctrls->use_rate            = 0;
        inter_comp_ctrls->pred0_to_pred1_mult = 2;

        break;
#else
#if !FTR_NEW_REF_PRUNING_CTRLS
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_dist1_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               1 /*DIFF*/,
                               1 /*WEDGE*/);
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_dist2_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               1 /*DIFF*/,
                               1 /*WEDGE*/);
#endif
        break;
    case 2:
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               1 /*DIFF*/,
                               1 /*WEDGE*/);
#if !FTR_NEW_REF_PRUNING_CTRLS
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_dist1_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               1 /*DIFF*/,
                               1 /*WEDGE*/);
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_dist2_comp_types,
                               1 /*AVG*/,
                               0 /*DIST*/,
                               0 /*DIFF*/,
                               0 /*WEDGE*/);
#endif
        break;
    case 3:
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               1 /*DIFF*/,
                               1 /*WEDGE*/);
#if !FTR_NEW_REF_PRUNING_CTRLS
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_dist1_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               1 /*DIFF*/,
                               0 /*WEDGE*/);
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_dist2_comp_types,
                               1 /*AVG*/,
                               0 /*DIST*/,
                               0 /*DIFF*/,
                               0 /*WEDGE*/);
#endif
        break;
    case 4:
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               1 /*DIFF*/,
                               1 /*WEDGE*/);
#if !FTR_NEW_REF_PRUNING_CTRLS
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_dist1_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               0 /*DIFF*/,
                               0 /*WEDGE*/);
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_dist2_comp_types,
                               1 /*AVG*/,
                               0 /*DIST*/,
                               0 /*DIFF*/,
                               0 /*WEDGE*/);
#endif
        break;
    case 5:
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               1 /*DIFF*/,
                               0 /*WEDGE*/);
#if !FTR_NEW_REF_PRUNING_CTRLS
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_dist1_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               0 /*DIFF*/,
                               0 /*WEDGE*/);
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_dist2_comp_types,
                               1 /*AVG*/,
                               0 /*DIST*/,
                               0 /*DIFF*/,
                               0 /*WEDGE*/);
#endif
        break;
    case 6:
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               0 /*DIFF*/,
                               0 /*WEDGE*/);
#if !FTR_NEW_REF_PRUNING_CTRLS
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_dist1_comp_types,
                               1 /*AVG*/,
                               1 /*DIST*/,
                               0 /*DIFF*/,
                               0 /*WEDGE*/);
        set_compound_to_inject(ctx,
                               inter_comp_ctrls->allowed_dist2_comp_types,
                               1 /*AVG*/,
                               0 /*DIST*/,
                               0 /*DIFF*/,
                               0 /*WEDGE*/);
#endif
        break;
#endif
    default: assert(0); break;
    }
}
void scale_nics(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr) {
    // minimum nics allowed
    uint32_t min_nics_stage1 = (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag &&
                                context_ptr->nic_ctrls.stage1_scaling_num)
        ? 2
        : 1;
    uint32_t min_nics_stage2 = (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag &&
                                context_ptr->nic_ctrls.stage2_scaling_num)
        ? 2
        : 1;
    uint32_t min_nics_stage3 = (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag &&
                                context_ptr->nic_ctrls.stage3_scaling_num)
        ? 2
        : 1;

    // Set the scaling numerators
    uint32_t stage1_scale_num = context_ptr->nic_ctrls.stage1_scaling_num;
    uint32_t stage2_scale_num = context_ptr->nic_ctrls.stage2_scaling_num;
    uint32_t stage3_scale_num = context_ptr->nic_ctrls.stage3_scaling_num;

    // The scaling denominator is 16 for all stages
#if CLN_MD_CAND_BUFF
    uint32_t scale_denum = MD_STAGE_NICS_SCAL_DENUM;
#else
    uint32_t scale_denum = 16;
#endif
    // no NIC setting should be done beyond this point
    for (uint8_t cidx = 0; cidx < CAND_CLASS_TOTAL; ++cidx) {
        context_ptr->md_stage_1_count[cidx] = MAX(
            min_nics_stage1,
            DIVIDE_AND_ROUND(context_ptr->md_stage_1_count[cidx] * stage1_scale_num, scale_denum));
        context_ptr->md_stage_2_count[cidx] = MAX(
            min_nics_stage2,
            DIVIDE_AND_ROUND(context_ptr->md_stage_2_count[cidx] * stage2_scale_num, scale_denum));
        context_ptr->md_stage_3_count[cidx] = MAX(
            min_nics_stage3,
            DIVIDE_AND_ROUND(context_ptr->md_stage_3_count[cidx] * stage3_scale_num, scale_denum));
    }
}

void set_md_stage_counts(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr) {
    // Step 1: derive bypass_stage1 flags
    if (context_ptr->md_staging_mode == MD_STAGING_MODE_1 ||
        context_ptr->md_staging_mode == MD_STAGING_MODE_2)
        memset(context_ptr->bypass_md_stage_1, EB_FALSE, CAND_CLASS_TOTAL);
    else
        memset(context_ptr->bypass_md_stage_1, EB_TRUE, CAND_CLASS_TOTAL);

    if (context_ptr->md_staging_mode == MD_STAGING_MODE_2)
        memset(context_ptr->bypass_md_stage_2, EB_FALSE, CAND_CLASS_TOTAL);
    else
        memset(context_ptr->bypass_md_stage_2, EB_TRUE, CAND_CLASS_TOTAL);

        // Step 2: set md_stage count
#if CLN_MD_CAND_BUFF
    uint8_t pic_type = pcs_ptr->slice_type == I_SLICE        ? 0
        : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 1
                                                             : 2;

    for (CandClass cand_class_it = CAND_CLASS_0; cand_class_it < CAND_CLASS_TOTAL;
         cand_class_it++) {
        context_ptr->md_stage_1_count[cand_class_it] = MD_STAGE_NICS[pic_type][cand_class_it];
        context_ptr->md_stage_2_count[cand_class_it] = MD_STAGE_NICS[pic_type][cand_class_it] >> 1;
        context_ptr->md_stage_3_count[cand_class_it] = MD_STAGE_NICS[pic_type][cand_class_it] >> 2;
    }
#else
    context_ptr->md_stage_1_count[CAND_CLASS_0] = (pcs_ptr->slice_type == I_SLICE) ? 64
        : (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag) ? 32
                                                               : 16;
    context_ptr->md_stage_1_count[CAND_CLASS_1] = (pcs_ptr->slice_type == I_SLICE) ? 0
        : (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag) ? 32
                                                               : 16;
    context_ptr->md_stage_1_count[CAND_CLASS_2] = (pcs_ptr->slice_type == I_SLICE) ? 0
        : (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag) ? 32
                                                               : 16;
    context_ptr->md_stage_1_count[CAND_CLASS_3] = (pcs_ptr->slice_type == I_SLICE) ? 16
        : (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag) ? 8
                                                               : 4;
    context_ptr->md_stage_2_count[CAND_CLASS_0] = (pcs_ptr->slice_type == I_SLICE) ? 32
        : (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag) ? 16
                                                               : 8;
    context_ptr->md_stage_2_count[CAND_CLASS_1] = (pcs_ptr->slice_type == I_SLICE) ? 0
        : (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag) ? 16
                                                               : 8;
    context_ptr->md_stage_2_count[CAND_CLASS_2] = (pcs_ptr->slice_type == I_SLICE) ? 0
        : (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag) ? 16
                                                               : 8;
    context_ptr->md_stage_2_count[CAND_CLASS_3] = (pcs_ptr->slice_type == I_SLICE) ? 8
        : (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag) ? 4
                                                               : 2;

    context_ptr->md_stage_3_count[CAND_CLASS_0] = (pcs_ptr->slice_type == I_SLICE) ? 16
        : (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag) ? 8
                                                               : 4;
    context_ptr->md_stage_3_count[CAND_CLASS_1] = (pcs_ptr->slice_type == I_SLICE) ? 0
        : (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag) ? 8
                                                               : 4;
    context_ptr->md_stage_3_count[CAND_CLASS_2] = (pcs_ptr->slice_type == I_SLICE) ? 0
        : (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag) ? 8
                                                               : 4;
    context_ptr->md_stage_3_count[CAND_CLASS_3] = (pcs_ptr->slice_type == I_SLICE) ? 4
        : (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag) ? 2
                                                               : 1;

#endif

    // no NIC setting should be done beyond this point
    // scale nics
    scale_nics(pcs_ptr, context_ptr);
    // Step 3: update count for md_stage_1 and d_stage_2 if bypassed (no NIC
    // setting should be done beyond this point)
    context_ptr->md_stage_2_count[CAND_CLASS_0] = context_ptr->bypass_md_stage_1[CAND_CLASS_0]
        ? context_ptr->md_stage_1_count[CAND_CLASS_0]
        : context_ptr->md_stage_2_count[CAND_CLASS_0];
    context_ptr->md_stage_2_count[CAND_CLASS_1] = context_ptr->bypass_md_stage_1[CAND_CLASS_1]
        ? context_ptr->md_stage_1_count[CAND_CLASS_1]
        : context_ptr->md_stage_2_count[CAND_CLASS_1];
    context_ptr->md_stage_2_count[CAND_CLASS_2] = context_ptr->bypass_md_stage_1[CAND_CLASS_2]
        ? context_ptr->md_stage_1_count[CAND_CLASS_2]
        : context_ptr->md_stage_2_count[CAND_CLASS_2];
    context_ptr->md_stage_2_count[CAND_CLASS_3] = context_ptr->bypass_md_stage_1[CAND_CLASS_3]
        ? context_ptr->md_stage_1_count[CAND_CLASS_3]
        : context_ptr->md_stage_2_count[CAND_CLASS_3];
    // TODO: use actual number of stages on the setting section and update using
    // the following logic.
    // stage1_cand_count[CAND_CLASS_i] = bypass_stage1 ?
    // stage2_cand_count[CAND_CLASS_i] : stage1_cand_count[CAND_CLASS_i];
    //  Update md_stage_3 NICs if md_stage_2 bypassed
    context_ptr->md_stage_3_count[CAND_CLASS_0] = context_ptr->bypass_md_stage_2[CAND_CLASS_0]
        ? context_ptr->md_stage_2_count[CAND_CLASS_0]
        : context_ptr->md_stage_3_count[CAND_CLASS_0];
    context_ptr->md_stage_3_count[CAND_CLASS_1] = context_ptr->bypass_md_stage_2[CAND_CLASS_1]
        ? context_ptr->md_stage_2_count[CAND_CLASS_1]
        : context_ptr->md_stage_3_count[CAND_CLASS_1];
    context_ptr->md_stage_3_count[CAND_CLASS_2] = context_ptr->bypass_md_stage_2[CAND_CLASS_2]
        ? context_ptr->md_stage_2_count[CAND_CLASS_2]
        : context_ptr->md_stage_3_count[CAND_CLASS_2];
    context_ptr->md_stage_3_count[CAND_CLASS_3] = context_ptr->bypass_md_stage_2[CAND_CLASS_3]
        ? context_ptr->md_stage_2_count[CAND_CLASS_3]
        : context_ptr->md_stage_3_count[CAND_CLASS_3];
    for (uint8_t cidx = 0; cidx < CAND_CLASS_TOTAL; ++cidx)
        context_ptr->md_stage_3_total_count += context_ptr->md_stage_3_count[cidx];
}
void sort_fast_cost_based_candidates(
    struct ModeDecisionContext *context_ptr, uint32_t input_buffer_start_idx,
    uint32_t
        input_buffer_count, //how many cand buffers to sort. one of the buffer can have max cost.
    uint32_t *cand_buff_indices) {
    ModeDecisionCandidateBuffer **buffer_ptr_array = context_ptr->candidate_buffer_ptr_array;
    uint32_t input_buffer_end_idx = input_buffer_start_idx + input_buffer_count - 1;
    uint32_t buffer_index, i, j;
    uint32_t k = 0;
    for (buffer_index = input_buffer_start_idx; buffer_index <= input_buffer_end_idx;
         buffer_index++, k++) {
        cand_buff_indices[k] = buffer_index;
    }
    for (i = 0; i < input_buffer_count - 1; ++i) {
        for (j = i + 1; j < input_buffer_count; ++j) {
            if (*(buffer_ptr_array[cand_buff_indices[j]]->fast_cost_ptr) <
                *(buffer_ptr_array[cand_buff_indices[i]]->fast_cost_ptr)) {
                buffer_index         = cand_buff_indices[i];
                cand_buff_indices[i] = (uint32_t)cand_buff_indices[j];
                cand_buff_indices[j] = (uint32_t)buffer_index;
            }
        }
    }
}
void sort_full_cost_based_candidates(struct ModeDecisionContext *context_ptr,
                                     uint32_t num_of_cand_to_sort, uint32_t *cand_buff_indices) {
    uint32_t                      i, j, index;
    ModeDecisionCandidateBuffer **buffer_ptr_array = context_ptr->candidate_buffer_ptr_array;
    for (i = 0; i < num_of_cand_to_sort - 1; ++i) {
        for (j = i + 1; j < num_of_cand_to_sort; ++j) {
            if (*(buffer_ptr_array[cand_buff_indices[j]]->full_cost_ptr) <
                *(buffer_ptr_array[cand_buff_indices[i]]->full_cost_ptr)) {
                index                = cand_buff_indices[i];
                cand_buff_indices[i] = (uint32_t)cand_buff_indices[j];
                cand_buff_indices[j] = (uint32_t)index;
            }
        }
    }
}
void construct_best_sorted_arrays_md_stage_3(
    struct ModeDecisionContext *context_ptr, ModeDecisionCandidateBuffer **buffer_ptr_array,
    uint32_t *best_candidate_index_array) { //best = union from all classes

    uint32_t best_candi = 0;
    for (CandClass class_i = CAND_CLASS_0; class_i < CAND_CLASS_TOTAL; class_i++)
        for (uint32_t candi = 0; candi < context_ptr->md_stage_3_count[class_i]; candi++)
            best_candidate_index_array[best_candi++] =
                context_ptr->cand_buff_indices[class_i][candi];

    assert(best_candi == context_ptr->md_stage_3_total_count);
    uint32_t fullReconCandidateCount = context_ptr->md_stage_3_total_count;

    // Only if chroma_at_last_md_stage
    if (context_ptr->chroma_at_last_md_stage) {
        uint32_t i, id;
        context_ptr->md_stage_3_total_intra_count = 0;
        for (i = 0; i < fullReconCandidateCount; ++i) {
            id               = best_candidate_index_array[i];
            uint8_t is_inter = (buffer_ptr_array[id]->candidate_ptr->type == INTER_MODE ||
                                buffer_ptr_array[id]->candidate_ptr->use_intrabc)
                ? EB_TRUE
                : EB_FALSE;
            context_ptr->md_stage_3_total_intra_count += !is_inter ? 1 : 0;
        }

        // Derive best_intra_cost and best_inter_cost
        context_ptr->best_intra_cost = MAX_MODE_COST;
        context_ptr->best_inter_cost = MAX_MODE_COST;
        for (i = 0; i < fullReconCandidateCount; ++i) {
            id               = best_candidate_index_array[i];
            uint8_t is_inter = (buffer_ptr_array[id]->candidate_ptr->type == INTER_MODE ||
                                buffer_ptr_array[id]->candidate_ptr->use_intrabc)
                ? EB_TRUE
                : EB_FALSE;
            if (!is_inter)
                if (*buffer_ptr_array[id]->full_cost_ptr < context_ptr->best_intra_cost)
                    context_ptr->best_intra_cost = *buffer_ptr_array[id]->full_cost_ptr;
            if (is_inter)
                if (*buffer_ptr_array[id]->full_cost_ptr < context_ptr->best_inter_cost)
                    context_ptr->best_inter_cost = *buffer_ptr_array[id]->full_cost_ptr;
        }

        // Update md_stage_3_total_intra_count based based on inter/intra cost deviation
        if ((context_ptr->best_inter_cost * context_ptr->chroma_at_last_md_stage_intra_th) <
            (context_ptr->best_intra_cost * 100))
            context_ptr->md_stage_3_total_intra_count = 0;
    }
}
void md_stage_0(

    PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
    ModeDecisionCandidateBuffer **candidate_buffer_ptr_array_base,
    ModeDecisionCandidate *fast_candidate_array, int32_t fast_candidate_start_index,
    int32_t fast_candidate_end_index, EbPictureBufferDesc *input_picture_ptr,
    uint32_t input_origin_index, uint32_t input_cb_origin_in_index,
    uint32_t input_cr_origin_in_index, BlkStruct *blk_ptr, uint32_t blk_origin_index,
    uint32_t blk_chroma_origin_index, uint32_t candidate_buffer_start_index, uint32_t max_buffers,
    EbBool scratch_buffer_pesent_flag) {
    int32_t  fast_loop_cand_index;
    uint32_t highest_cost_index;
    uint64_t highest_cost;
    EbBool   use_ssd = EB_FALSE;
    // Set MD Staging fast_loop_core settings
    context_ptr->md_staging_skip_interpolation_search = (context_ptr->interpolation_search_level ==
                                                         IFS_MDS0)
        ? EB_FALSE
        : EB_TRUE;
    context_ptr->md_staging_skip_chroma_pred          = EB_TRUE;
    // 2nd fast loop: src-to-recon
    highest_cost_index           = candidate_buffer_start_index;
    fast_loop_cand_index         = fast_candidate_end_index;
    context_ptr->mds0_best_cost  = (uint64_t)~0;
    context_ptr->mds0_best_class = 0;
    while (fast_loop_cand_index >= fast_candidate_start_index) {
        if (fast_candidate_array[fast_loop_cand_index].cand_class == context_ptr->target_class) {
            ModeDecisionCandidateBuffer *candidate_buffer =
                candidate_buffer_ptr_array_base[highest_cost_index];
#if CLN_MD_CANDS
            candidate_buffer->candidate_ptr = &fast_candidate_array[fast_loop_cand_index];
#else
            ModeDecisionCandidate *candidate_ptr = candidate_buffer->candidate_ptr =
                &fast_candidate_array[fast_loop_cand_index];
#endif
            // Initialize tx_depth
            candidate_buffer->candidate_ptr->tx_depth = 0;
#if !CLN_MD_CANDS
            if (!candidate_ptr->distortion_ready) {
#endif
                // Prediction
                fast_loop_core(candidate_buffer,
                               pcs_ptr,
                               context_ptr,
                               input_picture_ptr,
                               input_origin_index,
                               input_cb_origin_in_index,
                               input_cr_origin_in_index,
                               blk_ptr,
                               blk_origin_index,
                               blk_chroma_origin_index,
                               use_ssd);
#if !CLN_MD_CANDS
            }
#endif
            if (context_ptr->early_cand_elimination) {
                if (*candidate_buffer->fast_cost_ptr < context_ptr->mds0_best_cost) {
                    context_ptr->mds0_best_cost = *candidate_buffer->fast_cost_ptr;
                    context_ptr->mds0_best_class =
                        fast_candidate_array[fast_loop_cand_index].cand_class;
                }
            }

            // Find the buffer with the highest cost
            if (fast_loop_cand_index || scratch_buffer_pesent_flag) {
                // max_cost is volatile to prevent the compiler from loading 0xFFFFFFFFFFFFFF
                //   as a const at the early-out. Loading a large constant on intel x64 processors
                //   clogs the i-cache/intstruction decode. This still reloads the variable from
                //   the stack each pass, so a better solution would be to register the variable,
                //   but this might require asm.
                volatile uint64_t max_cost           = MAX_CU_COST;
                const uint64_t *  fast_cost_array    = context_ptr->fast_cost_array;
                const uint32_t    buffer_index_start = candidate_buffer_start_index;
                const uint32_t    buffer_index_end   = buffer_index_start + max_buffers;
                uint32_t          buffer_index;

                highest_cost_index = buffer_index_start;
                buffer_index       = buffer_index_start + 1;

                do {
                    highest_cost = fast_cost_array[highest_cost_index];
                    if (highest_cost == max_cost)
                        break;

                    if (fast_cost_array[buffer_index] > highest_cost)
                        highest_cost_index = buffer_index;
                } while (++buffer_index < buffer_index_end);
            }
        }
        --fast_loop_cand_index;
    }

    // Set the cost of the scratch canidate to max to get discarded @ the sorting phase
    *(candidate_buffer_ptr_array_base[highest_cost_index]->fast_cost_ptr) =
        (scratch_buffer_pesent_flag)
        ? MAX_CU_COST
        : *(candidate_buffer_ptr_array_base[highest_cost_index]->fast_cost_ptr);
}
void md_full_pel_search(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                        EbPictureBufferDesc *input_picture_ptr, EbPictureBufferDesc *ref_pic,
                        uint32_t input_origin_index, EbBool use_ssd, int16_t mvx, int16_t mvy,
                        int16_t search_position_start_x, int16_t search_position_end_x,
                        int16_t search_position_start_y, int16_t search_position_end_y,
                        int16_t sparse_search_step, int16_t *best_mvx, int16_t *best_mvy,
                        uint32_t *best_cost) {
    uint8_t hbd_mode_decision = context_ptr->hbd_mode_decision == EB_DUAL_BIT_MD
        ? EB_8_BIT_MD
        : context_ptr->hbd_mode_decision;
    // Mvcost params
    SUBPEL_MOTION_SEARCH_PARAMS  ms_params_struct;
    SUBPEL_MOTION_SEARCH_PARAMS *ms_params = &ms_params_struct;
    FrameHeader *                frm_hdr   = &pcs_ptr->parent_pcs_ptr->frm_hdr;

    uint32_t rdmult = use_ssd
        ? context_ptr->full_lambda_md[hbd_mode_decision ? EB_10_BIT_MD : EB_8_BIT_MD]
        : context_ptr->fast_lambda_md[hbd_mode_decision ? EB_10_BIT_MD : EB_8_BIT_MD];
    svt_init_mv_cost_params(&ms_params->mv_cost_params,
                            context_ptr,
                            &context_ptr->ref_mv,
                            frm_hdr->quantization_params.base_q_idx,
                            rdmult,
                            hbd_mode_decision);
    uint32_t cost;
    // Search area adjustment
    if ((context_ptr->blk_origin_x + (mvx >> 3) + search_position_start_x) <
        (-ref_pic->origin_x + 1))
        search_position_start_x = (-ref_pic->origin_x + 1) -
            (context_ptr->blk_origin_x + (mvx >> 3));

    if ((context_ptr->blk_origin_x + context_ptr->blk_geom->bwidth + (mvx >> 3) +
         search_position_end_x) > (ref_pic->origin_x + ref_pic->max_width - 1))
        search_position_end_x = (ref_pic->origin_x + ref_pic->max_width - 1) -
            (context_ptr->blk_origin_x + context_ptr->blk_geom->bwidth + (mvx >> 3));

    if ((context_ptr->blk_origin_y + (mvy >> 3) + search_position_start_y) <
        (-ref_pic->origin_y + 1))
        search_position_start_y = (-ref_pic->origin_y + 1) -
            (context_ptr->blk_origin_y + (mvy >> 3));

    if ((context_ptr->blk_origin_y + context_ptr->blk_geom->bheight + (mvy >> 3) +
         search_position_end_y) > (ref_pic->origin_y + ref_pic->max_height - 1))
        search_position_end_y = (ref_pic->origin_y + ref_pic->max_height - 1) -
            (context_ptr->blk_origin_y + context_ptr->blk_geom->bheight + (mvy >> 3));

    for (int32_t refinement_pos_x = search_position_start_x;
         refinement_pos_x <= search_position_end_x;
         refinement_pos_x = refinement_pos_x + sparse_search_step) {
        for (int32_t refinement_pos_y = search_position_start_y;
             refinement_pos_y <= search_position_end_y;
             refinement_pos_y = refinement_pos_y + sparse_search_step) {
            // If sparse search level_1
            if (sparse_search_step == 2) {
                // If search level_0 previously performed
                if (context_ptr->md_sq_me_ctrls.sprs_lev0_enabled &&
                    context_ptr->md_sq_me_ctrls.sprs_lev0_step == 4) {
                    // If level_0 range
                    if ((refinement_pos_x + (mvx >> 3)) >= context_ptr->sprs_lev0_start_x &&
                        (refinement_pos_x + (mvx >> 3)) <= context_ptr->sprs_lev0_end_x &&
                        (refinement_pos_y + (mvy >> 3)) >= context_ptr->sprs_lev0_start_y &&
                        (refinement_pos_y + (mvy >> 3)) <= context_ptr->sprs_lev0_end_y)
                        // If level_0 position
                        if (refinement_pos_x % 4 == 0 && refinement_pos_y % 4 == 0)
                            continue;
                }
            }
            int32_t ref_origin_index = ref_pic->origin_x +
                (context_ptr->blk_origin_x + (mvx >> 3) + refinement_pos_x) +
                (context_ptr->blk_origin_y + (mvy >> 3) + ref_pic->origin_y + refinement_pos_y) *
                    ref_pic->stride_y;
            if (use_ssd) {
                EbSpatialFullDistType spatial_full_dist_type_fun = hbd_mode_decision
                    ? svt_full_distortion_kernel16_bits
                    : svt_spatial_full_distortion_kernel;

                cost = (uint32_t)spatial_full_dist_type_fun(input_picture_ptr->buffer_y,
                                                            input_origin_index,
                                                            input_picture_ptr->stride_y,
                                                            ref_pic->buffer_y,
                                                            ref_origin_index,
                                                            ref_pic->stride_y,
                                                            context_ptr->blk_geom->bwidth,
                                                            context_ptr->blk_geom->bheight);
            } else {
                assert((context_ptr->blk_geom->bwidth >> 3) < 17);

                if (hbd_mode_decision) {
                    cost = sad_16b_kernel(
                        ((uint16_t *)input_picture_ptr->buffer_y) + input_origin_index,
                        input_picture_ptr->stride_y,
                        ((uint16_t *)ref_pic->buffer_y) + ref_origin_index,
                        ref_pic->stride_y,
                        context_ptr->blk_geom->bheight,
                        context_ptr->blk_geom->bwidth);
                } else {
                    cost = svt_nxm_sad_kernel_sub_sampled(
                        input_picture_ptr->buffer_y + input_origin_index,
                        input_picture_ptr->stride_y,
                        ref_pic->buffer_y + ref_origin_index,
                        ref_pic->stride_y,
                        context_ptr->blk_geom->bheight,
                        context_ptr->blk_geom->bwidth);
                }
            }
            MV best_mv;
            best_mv.col = mvx + (refinement_pos_x * 8);
            best_mv.row = mvy + (refinement_pos_y * 8);
            cost += fp_mv_err_cost(&best_mv, &ms_params->mv_cost_params);
            if (cost < *best_cost) {
                *best_mvx  = mvx + (refinement_pos_x * 8);
                *best_mvy  = mvy + (refinement_pos_y * 8);
                *best_cost = cost;
            }
        }
    }
}
void    av1_set_ref_frame(MvReferenceFrame *rf, int8_t ref_frame_type);
uint8_t get_max_drl_index(uint8_t refmvCnt, PredictionMode mode);
uint8_t is_me_data_present(struct ModeDecisionContext *context_ptr, const MeSbResults *me_results,
                           uint8_t list_idx, uint8_t ref_idx);
// Derive me_sb_addr and me_block_offset used to access ME_MV
void derive_me_offsets(const SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr,
                       ModeDecisionContext *context_ptr) {
    // @ this stage NSQ block(s) are inheriting SQ block(s) ME results; MV(s), pruning PA_ME results

    // Get parent_depth_idx_mds
    uint16_t parent_depth_idx_mds = 0;
    if (context_ptr->blk_geom->sq_size <
        ((scs_ptr->seq_header.sb_size == BLOCK_128X128) ? 128 : 64))
        //Set parent to be considered
        parent_depth_idx_mds = (context_ptr->blk_geom->sqi_mds -
                                (context_ptr->blk_geom->quadi - 3) *
                                    ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128]
                                                   [context_ptr->blk_geom->depth]) -
            parent_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128]
                               [context_ptr->blk_geom->depth];

    const BlockGeom *sq_blk_geom = (context_ptr->blk_geom->bwidth != context_ptr->blk_geom->bheight)
        ? get_blk_geom_mds(context_ptr->blk_geom
                               ->sqi_mds) // Use parent block SQ info as ME not performed for NSQ
        : (context_ptr->blk_geom->bwidth == 4 ||
           context_ptr->blk_geom->bheight ==
               4) // Use parent_depth SQ block info as ME not performed for 4x4
        ? get_blk_geom_mds(parent_depth_idx_mds)
        : context_ptr->blk_geom;

    context_ptr->geom_offset_x = 0;
    context_ptr->geom_offset_y = 0;

    if (scs_ptr->seq_header.sb_size == BLOCK_128X128) {
        uint32_t me_sb_size         = scs_ptr->sb_sz;
        uint32_t me_pic_width_in_sb = (pcs_ptr->parent_pcs_ptr->aligned_width + scs_ptr->sb_sz -
                                       1) /
            me_sb_size;
        uint32_t me_sb_x             = (context_ptr->blk_origin_x / me_sb_size);
        uint32_t me_sb_y             = (context_ptr->blk_origin_y / me_sb_size);
        context_ptr->me_sb_addr      = me_sb_x + me_sb_y * me_pic_width_in_sb;
        context_ptr->geom_offset_x   = (me_sb_x & 0x1) * me_sb_size;
        context_ptr->geom_offset_y   = (me_sb_y & 0x1) * me_sb_size;
        context_ptr->me_block_offset = (uint32_t)
            me_idx_128x128[((context_ptr->geom_offset_y / me_sb_size) * 2) +
                           (context_ptr->geom_offset_x / me_sb_size)]
                          [context_ptr->blk_geom->blkidx_mds];
    } else {
        context_ptr->me_sb_addr      = context_ptr->sb_ptr->index;
        context_ptr->me_block_offset = me_idx[context_ptr->blk_geom->blkidx_mds];
    }

    if (sq_blk_geom->bwidth == 128 || sq_blk_geom->bheight == 128) {
        context_ptr->me_block_offset = 0;
    }
    assert(context_ptr->me_block_offset != (uint32_t)(-1));
    context_ptr->me_cand_offset = context_ptr->me_block_offset * MAX_PA_ME_CAND;
}
#define MAX_MD_NSQ_SARCH_MVC_CNT 5
void md_nsq_motion_search(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                          EbPictureBufferDesc *input_picture_ptr, uint32_t input_origin_index,
                          uint8_t list_idx, uint8_t ref_idx, const MeSbResults *me_results,
                          int16_t *me_mv_x, int16_t *me_mv_y) {
    // Step 0: derive the MVC list for the NSQ search; 1 SQ MV (default MV for NSQ) and up to 4 sub-block MV(s) (e.g. if 16x8 then 2 8x8, if 32x8 then 4 8x8)
    int16_t mvc_x_array[MAX_MD_NSQ_SARCH_MVC_CNT];
    int16_t mvc_y_array[MAX_MD_NSQ_SARCH_MVC_CNT];
    int8_t  mvc_count = 0;
    // SQ MV (default MVC for NSQ)
    mvc_x_array[mvc_count] = *me_mv_x;
    mvc_y_array[mvc_count] = *me_mv_y;
    mvc_count++;
    if ((context_ptr->blk_geom->bwidth != 4 && context_ptr->blk_geom->bheight != 4) &&
        context_ptr->blk_geom->sq_size >= 16) {
        uint8_t min_size = MIN(context_ptr->blk_geom->bwidth, context_ptr->blk_geom->bheight);
        // Derive the sub-block(s) MVs (additional MVC for NSQ)
        for (uint32_t block_index = 0;
             block_index < pcs_ptr->parent_pcs_ptr->max_number_of_pus_per_sb;
             block_index++) {
            if ((min_size == partition_width[block_index] ||
                 min_size == partition_height[block_index]) &&
                ((pu_search_index_map[block_index][0] >=
                  (context_ptr->blk_geom->origin_x - context_ptr->geom_offset_x)) &&
                 (pu_search_index_map[block_index][0] < context_ptr->blk_geom->bwidth +
                      (context_ptr->blk_geom->origin_x - context_ptr->geom_offset_x))) &&
                ((pu_search_index_map[block_index][1] >=
                  (context_ptr->blk_geom->origin_y - context_ptr->geom_offset_y)) &&
                 (pu_search_index_map[block_index][1] < context_ptr->blk_geom->bheight +
                      (context_ptr->blk_geom->origin_y - context_ptr->geom_offset_y)))) {
                if (list_idx == 0) {
                    mvc_x_array[mvc_count] =
                        (me_results->me_mv_array[block_index * MAX_PA_ME_MV + ref_idx].x_mv) << 1;
                    mvc_y_array[mvc_count] =
                        (me_results->me_mv_array[block_index * MAX_PA_ME_MV + ref_idx].y_mv) << 1;
                } else {
                    mvc_x_array[mvc_count] =
                        (me_results->me_mv_array[block_index * MAX_PA_ME_MV + 4 + ref_idx].x_mv)
                        << 1;
                    mvc_y_array[mvc_count] =
                        (me_results->me_mv_array[block_index * MAX_PA_ME_MV + 4 + ref_idx].y_mv)
                        << 1;
                }
                mvc_count++;
            }
        }
    }

    // Search Center
    int16_t  search_center_mvx  = mvc_x_array[0];
    int16_t  search_center_mvy  = mvc_y_array[0];
    uint32_t search_center_cost = (uint32_t)~0;

    uint8_t              hbd_mode_decision = context_ptr->hbd_mode_decision == EB_DUAL_BIT_MD
                     ? EB_8_BIT_MD
                     : context_ptr->hbd_mode_decision;
    EbReferenceObject *  ref_obj = pcs_ptr->ref_pic_ptr_array[list_idx][ref_idx]->object_ptr;
    EbPictureBufferDesc *ref_pic = hbd_mode_decision ? ref_obj->reference_picture16bit
                                                     : ref_obj->reference_picture;
    for (int16_t mvc_index = 0; mvc_index < mvc_count; mvc_index++) {
        // Round-up the search center to the closest integer
        mvc_x_array[mvc_index] = (mvc_x_array[mvc_index] + 4) & ~0x07;
        mvc_y_array[mvc_index] = (mvc_y_array[mvc_index] + 4) & ~0x07;

        md_full_pel_search(pcs_ptr,
                           context_ptr,
                           input_picture_ptr,
                           ref_pic,
                           input_origin_index,
                           context_ptr->md_nsq_motion_search_ctrls.use_ssd,
                           mvc_x_array[mvc_index],
                           mvc_y_array[mvc_index],
                           0,
                           0,
                           0,
                           0,
                           1,
                           &search_center_mvx,
                           &search_center_mvy,
                           &search_center_cost);
    }

    *me_mv_x                  = search_center_mvx;
    *me_mv_y                  = search_center_mvy;
    int16_t  best_search_mvx  = (int16_t)~0;
    int16_t  best_search_mvy  = (int16_t)~0;
    uint32_t best_search_cost = (uint32_t)~0;

    md_full_pel_search(pcs_ptr,
                       context_ptr,
                       input_picture_ptr,
                       ref_pic,
                       input_origin_index,
                       context_ptr->md_nsq_motion_search_ctrls.use_ssd,
                       search_center_mvx,
                       search_center_mvy,
                       -(context_ptr->md_nsq_motion_search_ctrls.full_pel_search_width >> 1),
                       +(context_ptr->md_nsq_motion_search_ctrls.full_pel_search_width >> 1),
                       -(context_ptr->md_nsq_motion_search_ctrls.full_pel_search_height >> 1),
                       +(context_ptr->md_nsq_motion_search_ctrls.full_pel_search_height >> 1),
                       1,
                       &best_search_mvx,
                       &best_search_mvy,
                       &best_search_cost);
    if (best_search_cost < search_center_cost) {
        *me_mv_x = best_search_mvx;
        *me_mv_y = best_search_mvy;
    }
}
/*
   clips input MV (in 1/8 precision) to stay within boundaries of a given ref pic
*/
void clip_mv_on_pic_boundary(int32_t blk_origin_x, int32_t blk_origin_y, int32_t bwidth,
                             int32_t bheight, EbPictureBufferDesc *ref_pic, int16_t *mvx,
                             int16_t *mvy) {
    if (blk_origin_x + (*mvx >> 3) + bwidth > ref_pic->max_width + ref_pic->origin_x)
        *mvx = (ref_pic->max_width - blk_origin_x) << 3;

    if (blk_origin_y + (*mvy >> 3) + bheight > ref_pic->max_height + ref_pic->origin_y)
        *mvy = (ref_pic->max_height - blk_origin_y) << 3;

    if (blk_origin_x + (*mvx >> 3) < -ref_pic->origin_x)
        *mvx = (-blk_origin_x - bwidth) << 3;

    if (blk_origin_y + (*mvy >> 3) < -ref_pic->origin_y)
        *mvy = (-blk_origin_y - bheight) << 3;
}
/*
 * Check the size of the spatial MVs and MVPs of the given block
 *
 * Return a motion category, based on the MV size.
 */
uint8_t check_spatial_mv_size(ModeDecisionContext *ctx, uint8_t list_idx, uint8_t ref_idx,
                              int16_t *me_mv_x, int16_t *me_mv_y) {
    uint8_t search_area_multiplier = 0;

    // Iterate over all MVPs; if large, set high search_area_multiplier
    for (int8_t mvp_index = 0; mvp_index < ctx->mvp_count[list_idx][ref_idx]; mvp_index++) {
        if (ctx->mvp_array[list_idx][ref_idx][mvp_index].col > HIGH_SPATIAL_MV_TH ||
            ctx->mvp_array[list_idx][ref_idx][mvp_index].row > HIGH_SPATIAL_MV_TH ||
            *me_mv_x > HIGH_SPATIAL_MV_TH || *me_mv_y > HIGH_SPATIAL_MV_TH) {
            search_area_multiplier = MAX(3, search_area_multiplier);
            return search_area_multiplier; // reached MAX value already
        } else if (ctx->mvp_array[list_idx][ref_idx][mvp_index].col > MEDIUM_SPATIAL_MV_TH ||
                   ctx->mvp_array[list_idx][ref_idx][mvp_index].row > MEDIUM_SPATIAL_MV_TH ||
                   *me_mv_x > MEDIUM_SPATIAL_MV_TH || *me_mv_y > MEDIUM_SPATIAL_MV_TH) {
            search_area_multiplier = MAX(2, search_area_multiplier);
        } else if (ctx->mvp_array[list_idx][ref_idx][mvp_index].col > LOW_SPATIAL_MV_TH ||
                   ctx->mvp_array[list_idx][ref_idx][mvp_index].row > LOW_SPATIAL_MV_TH ||
                   *me_mv_x > LOW_SPATIAL_MV_TH || *me_mv_y > LOW_SPATIAL_MV_TH) {
            search_area_multiplier = MAX(1, search_area_multiplier);
        }
    }
    return search_area_multiplier;
}

/*
 * Check the size of the temporal MVs
 *
 * Return a motion category, based on the MV size.
 */
uint8_t check_temporal_mv_size(PictureControlSet *pcs, ModeDecisionContext *ctx) {
    uint8_t search_area_multiplier = 0;

    Av1Common * cm             = pcs->parent_pcs_ptr->av1_cm;
    int32_t     mi_row         = ctx->blk_origin_y >> MI_SIZE_LOG2;
    int32_t     mi_col         = ctx->blk_origin_x >> MI_SIZE_LOG2;
    TPL_MV_REF *prev_frame_mvs = pcs->tpl_mvs + (mi_row >> 1) * (cm->mi_stride >> 1) +
        (mi_col >> 1);
    TPL_MV_REF *mv = prev_frame_mvs;
    if (prev_frame_mvs->mfmv0.as_int != INVALID_MV) {
        if (ABS(mv->mfmv0.as_mv.row) > MEDIUM_TEMPORAL_MV_TH ||
            ABS(mv->mfmv0.as_mv.col) > MEDIUM_TEMPORAL_MV_TH) {
            search_area_multiplier = MAX(2, search_area_multiplier);
        } else if (ABS(mv->mfmv0.as_mv.row) > LOW_TEMPORAL_MV_TH ||
                   ABS(mv->mfmv0.as_mv.col) > LOW_TEMPORAL_MV_TH) {
            search_area_multiplier = MAX(1, search_area_multiplier);
        }
    }

    return search_area_multiplier;
}
/*
 * Detect if block has high motion, and if so, perform an expanded ME search.
 */
void md_sq_motion_search(PictureControlSet *pcs, ModeDecisionContext *ctx,
                         EbPictureBufferDesc *input_picture_ptr, uint32_t input_origin_index,
                         uint8_t list_idx, uint8_t ref_idx, int16_t *me_mv_x, int16_t *me_mv_y) {
    uint8_t              hbd_mode_decision = ctx->hbd_mode_decision == EB_DUAL_BIT_MD ? EB_8_BIT_MD
                                                                                      : ctx->hbd_mode_decision;
    EbReferenceObject *  ref_obj           = pcs->ref_pic_ptr_array[list_idx][ref_idx]->object_ptr;
    EbPictureBufferDesc *ref_pic           = hbd_mode_decision ? ref_obj->reference_picture16bit
                                                               : ref_obj->reference_picture;

    MdSqMotionSearchCtrls *md_sq_me_ctrls = &ctx->md_sq_me_ctrls;
    uint16_t               dist           = ABS(
        (int16_t)(pcs->picture_number - pcs->parent_pcs_ptr->ref_pic_poc_array[list_idx][ref_idx]));
    uint8_t search_area_multiplier = 0;

    // Get pa_me distortion and MVs
    int16_t  pa_me_mvx  = (int16_t)~0;
    int16_t  pa_me_mvy  = (int16_t)~0;
    uint32_t pa_me_cost = (uint32_t)~0;
    md_full_pel_search(pcs,
                       ctx,
                       input_picture_ptr,
                       ref_pic,
                       input_origin_index,
                       md_sq_me_ctrls->use_ssd,
                       *me_mv_x,
                       *me_mv_y,
                       0,
                       0,
                       0,
                       0,
                       1,
                       &pa_me_mvx,
                       &pa_me_mvy,
                       &pa_me_cost);

    // Identify potential high active block(s) and ME failure using 2 checks : (1) high ME_MV distortion, (2) active co - located block for non - intra ref(Temporal - MV(s)) or active surrounding block(s) for intra ref(Spatial - MV(s))
    if (ctx->blk_geom->sq_size <= 64) {
        uint32_t fast_lambda = ctx->hbd_mode_decision ? ctx->fast_lambda_md[EB_10_BIT_MD]
                                                      : ctx->fast_lambda_md[EB_8_BIT_MD];

        // Check if pa_me distortion is above the per-pixel threshold.  Rate is set to 16.
        if (RDCOST(fast_lambda, 16, pa_me_cost) >
            RDCOST(fast_lambda,
                   16,
                   md_sq_me_ctrls->pame_distortion_th * ctx->blk_geom->bwidth *
                       ctx->blk_geom->bheight)) {
            ref_obj = (EbReferenceObject *)pcs->ref_pic_ptr_array[list_idx][ref_idx]->object_ptr;

            search_area_multiplier = !(ref_obj == NULL || ref_obj->frame_type == KEY_FRAME ||
                                       ref_obj->frame_type == INTRA_ONLY_FRAME)
                ? check_temporal_mv_size(pcs, ctx)
                : check_spatial_mv_size(ctx, list_idx, ref_idx, me_mv_x, me_mv_y);
        }
    }

    // If high motion was detected, perform an expanded ME search
    if (search_area_multiplier) {
        int16_t  best_search_mvx  = (int16_t)~0;
        int16_t  best_search_mvy  = (int16_t)~0;
        uint32_t best_search_cost = (uint32_t)~0;

        int8_t round_up = ((dist % 8) == 0) ? 0 : 1;
        dist = ((dist * 5) / 8) + round_up; // factor to slow down the search region growth to MAX

        // Sparse-search Level_0
        if (md_sq_me_ctrls->sprs_lev0_enabled) {
            uint16_t sprs_lev0_w = (md_sq_me_ctrls->sprs_lev0_multiplier *
                                    MIN((md_sq_me_ctrls->sprs_lev0_w * search_area_multiplier *
                                         dist),
                                        md_sq_me_ctrls->max_sprs_lev0_w)) /
                100;
            uint16_t sprs_lev0_h = (md_sq_me_ctrls->sprs_lev0_multiplier *
                                    MIN((md_sq_me_ctrls->sprs_lev0_h * search_area_multiplier *
                                         dist),
                                        md_sq_me_ctrls->max_sprs_lev0_h)) /
                100;
            uint8_t sprs_lev0_step = md_sq_me_ctrls->sprs_lev0_step;

            // Derive start/end position of sparse search (must be a multiple of the step size)
            int16_t search_position_start_x = -(((sprs_lev0_w >> 1) / sprs_lev0_step) *
                                                sprs_lev0_step);
            int16_t search_position_end_x   = +(((sprs_lev0_w >> 1) / sprs_lev0_step) *
                                              sprs_lev0_step);
            int16_t search_position_start_y = -(((sprs_lev0_h >> 1) / sprs_lev0_step) *
                                                sprs_lev0_step);
            int16_t search_position_end_y   = +(((sprs_lev0_h >> 1) / sprs_lev0_step) *
                                              sprs_lev0_step);

            ctx->sprs_lev0_start_x = (*me_mv_x >> 3) + search_position_start_x;
            ctx->sprs_lev0_end_x   = (*me_mv_x >> 3) + search_position_end_x;
            ctx->sprs_lev0_start_y = (*me_mv_y >> 3) + search_position_start_y;
            ctx->sprs_lev0_end_y   = (*me_mv_y >> 3) + search_position_end_y;

            md_full_pel_search(pcs,
                               ctx,
                               input_picture_ptr,
                               ref_pic,
                               input_origin_index,
                               md_sq_me_ctrls->use_ssd,
                               *me_mv_x,
                               *me_mv_y,
                               search_position_start_x,
                               search_position_end_x,
                               search_position_start_y,
                               search_position_end_y,
                               sprs_lev0_step,
                               &best_search_mvx,
                               &best_search_mvy,
                               &best_search_cost);

            *me_mv_x = best_search_mvx;
            *me_mv_y = best_search_mvy;
        }

        // Sparse-search Level_1
        if (md_sq_me_ctrls->sprs_lev1_enabled) {
            uint16_t sprs_lev1_w = (md_sq_me_ctrls->sprs_lev1_multiplier *
                                    MIN((md_sq_me_ctrls->sprs_lev1_w * search_area_multiplier *
                                         dist),
                                        md_sq_me_ctrls->max_sprs_lev1_w)) /
                100;
            uint16_t sprs_lev1_h = (md_sq_me_ctrls->sprs_lev1_multiplier *
                                    MIN((md_sq_me_ctrls->sprs_lev1_h * search_area_multiplier *
                                         dist),
                                        md_sq_me_ctrls->max_sprs_lev1_h)) /
                100;
            uint8_t sprs_lev1_step = md_sq_me_ctrls->sprs_lev1_step;

            // Derive start/end position of sparse search (must be a multiple of the step size)
            int16_t search_position_start_x = -(((sprs_lev1_w >> 1) / sprs_lev1_step) *
                                                sprs_lev1_step);
            int16_t search_position_end_x   = +(((sprs_lev1_w >> 1) / sprs_lev1_step) *
                                              sprs_lev1_step);
            int16_t search_position_start_y = -(((sprs_lev1_h >> 1) / sprs_lev1_step) *
                                                sprs_lev1_step);
            int16_t search_position_end_y   = +(((sprs_lev1_h >> 1) / sprs_lev1_step) *
                                              sprs_lev1_step);

            search_position_start_x = (search_position_start_x % 4 == 0)
                ? search_position_start_x - 2
                : search_position_start_x;
            search_position_end_x   = (search_position_end_x % 4 == 0) ? search_position_end_x + 2
                                                                       : search_position_end_x;
            search_position_start_y = (search_position_start_y % 4 == 0)
                ? search_position_start_y - 2
                : search_position_start_y;
            search_position_end_y   = (search_position_end_y % 4 == 0) ? search_position_end_y + 2
                                                                       : search_position_end_y;

            md_full_pel_search(pcs,
                               ctx,
                               input_picture_ptr,
                               ref_pic,
                               input_origin_index,
                               md_sq_me_ctrls->use_ssd,
                               *me_mv_x,
                               *me_mv_y,
                               search_position_start_x,
                               search_position_end_x,
                               search_position_start_y,
                               search_position_end_y,
                               sprs_lev1_step,
                               &best_search_mvx,
                               &best_search_mvy,
                               &best_search_cost);

            *me_mv_x = best_search_mvx;
            *me_mv_y = best_search_mvy;
        }

        // Sparse-search Level_2
        if (md_sq_me_ctrls->sprs_lev2_enabled) {
            md_full_pel_search(
                pcs,
                ctx,
                input_picture_ptr,
                ref_pic,
                input_origin_index,
                md_sq_me_ctrls->use_ssd,
                *me_mv_x,
                *me_mv_y,
                -(((md_sq_me_ctrls->sprs_lev2_w >> 1) / md_sq_me_ctrls->sprs_lev2_step) *
                  md_sq_me_ctrls->sprs_lev2_step),
                +(((md_sq_me_ctrls->sprs_lev2_w >> 1) / md_sq_me_ctrls->sprs_lev2_step) *
                  md_sq_me_ctrls->sprs_lev2_step),
                -(((md_sq_me_ctrls->sprs_lev2_h >> 1) / md_sq_me_ctrls->sprs_lev2_step) *
                  md_sq_me_ctrls->sprs_lev2_step),
                +(((md_sq_me_ctrls->sprs_lev2_h >> 1) / md_sq_me_ctrls->sprs_lev2_step) *
                  md_sq_me_ctrls->sprs_lev2_step),
                md_sq_me_ctrls->sprs_lev2_step,
                &best_search_mvx,
                &best_search_mvy,
                &best_search_cost);

            *me_mv_x = best_search_mvx;
            *me_mv_y = best_search_mvy;
        }
        // Check that the resulting MV is within the AV1 limits
        check_mv_validity(*me_mv_x, *me_mv_y, 0);
    }
}
/*
 * Perform 1/2-Pel, 1/4-Pel, and 1/8-Pel search around the best Full-Pel position
 */
int md_subpel_search(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                     MdSubPelSearchCtrls md_subpel_ctrls, EbPictureBufferDesc *input_picture_ptr,
                     uint8_t list_idx, uint8_t ref_idx, int16_t *me_mv_x, int16_t *me_mv_y) {
    FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;

    const Av1Common *const cm = pcs_ptr->parent_pcs_ptr->av1_cm;
    MacroBlockD *          xd = context_ptr->blk_ptr->av1xd;

    // ref_mv is used to calculate the cost of the motion vector
    MV ref_mv;
    ref_mv.col = context_ptr->ref_mv.col;
    ref_mv.row = context_ptr->ref_mv.row;
    // High level params
    SUBPEL_MOTION_SEARCH_PARAMS  ms_params_struct;
    SUBPEL_MOTION_SEARCH_PARAMS *ms_params = &ms_params_struct;

    ms_params->allow_hp = md_subpel_ctrls.eight_pel_search_enabled &&
        pcs_ptr->parent_pcs_ptr->frm_hdr.allow_high_precision_mv;
    ms_params->forced_stop = EIGHTH_PEL;
    ms_params->iters_per_step =
        md_subpel_ctrls
            .subpel_iters_per_step; // Maximum number of steps in logarithmic subpel search before giving up.
    ms_params->cost_list = NULL;
    // Derive mv_limits (TODO Hsan_Subpel should be derived under md_context @ eack block)
    // Set up limit values for MV components.
    // Mv beyond the range do not produce new/different prediction block.
    MvLimits mv_limits;
    int      mi_row    = xd->mi_row;
    int      mi_col    = xd->mi_col;
    int      mi_width  = mi_size_wide[context_ptr->blk_geom->bsize];
    int      mi_height = mi_size_high[context_ptr->blk_geom->bsize];
    mv_limits.row_min  = -(((mi_row + mi_height) * MI_SIZE) + AOM_INTERP_EXTEND);
    mv_limits.col_min  = -(((mi_col + mi_width) * MI_SIZE) + AOM_INTERP_EXTEND);
    mv_limits.row_max  = (cm->mi_rows - mi_row) * MI_SIZE + AOM_INTERP_EXTEND;
    mv_limits.col_max  = (cm->mi_cols - mi_col) * MI_SIZE + AOM_INTERP_EXTEND;
    svt_av1_set_mv_search_range(&mv_limits, &ref_mv);
    svt_av1_set_subpel_mv_search_range(&ms_params->mv_limits, (FullMvLimits *)&mv_limits, &ref_mv);

    // Mvcost params
    svt_init_mv_cost_params(&ms_params->mv_cost_params,
                            context_ptr,
                            &ref_mv,
                            frm_hdr->quantization_params.base_q_idx,
                            context_ptr->full_lambda_md[EB_8_BIT_MD],
                            0); // 10BIT not supported
    // Subpel variance params
    ms_params->var_params.vfp                = &mefn_ptr[context_ptr->blk_geom->bsize];
    ms_params->var_params.subpel_search_type = md_subpel_ctrls.subpel_search_type;
    ms_params->var_params.w                  = block_size_wide[context_ptr->blk_geom->bsize];
    ms_params->var_params.h                  = block_size_high[context_ptr->blk_geom->bsize];

    // Ref and src buffers
    MSBuffers *ms_buffers = &ms_params->var_params.ms_buffers;

    // Ref buffer
    EbReferenceObject *  ref_obj = pcs_ptr->ref_pic_ptr_array[list_idx][ref_idx]->object_ptr;
    EbPictureBufferDesc *ref_pic = ref_obj->reference_picture; // 10BIT not supported
    int32_t              ref_origin_index = ref_pic->origin_x + context_ptr->blk_origin_x +
        (context_ptr->blk_origin_y + ref_pic->origin_y) * ref_pic->stride_y;

    // Ref buffer
    struct svt_buf_2d  ref_struct;
    struct svt_buf_2d *ref = &ref_struct;
    ref->buf               = ref_pic->buffer_y + ref_origin_index;
    ref->buf0              = NULL;
    ref->width             = ref_pic->width;
    ref->height            = ref_pic->height;
    ref->stride            = ref_pic->stride_y;
    ms_buffers->ref        = ref;

    // Src buffer
    uint32_t input_origin_index = (context_ptr->blk_origin_y + input_picture_ptr->origin_y) *
            input_picture_ptr->stride_y +
        (context_ptr->blk_origin_x + input_picture_ptr->origin_x);
    struct svt_buf_2d  src_struct;
    struct svt_buf_2d *src = &src_struct;
    src->buf               = input_picture_ptr->buffer_y + input_origin_index;
    src->buf0              = NULL;
    src->width             = input_picture_ptr->width;
    src->height            = input_picture_ptr->height;
    src->stride            = input_picture_ptr->stride_y;
    ms_buffers->src        = src;

    svt_av1_set_ms_compound_refs(ms_buffers, NULL, NULL, 0, 0);
    ms_buffers->wsrc      = NULL;
    ms_buffers->obmc_mask = NULL;

    int_mv best_mv;
    best_mv.as_mv.col = *me_mv_x >> 3;
    best_mv.as_mv.row = *me_mv_y >> 3;

    int          not_used        = 0;
    MV           subpel_start_mv = get_mv_from_fullmv(&best_mv.as_fullmv);
    unsigned int pred_sse        = 0; // not used
#if FTR_PRUNED_SUBPEL_TREE
    // Assign which subpel search method to use
    fractional_mv_step_fp *subpel_search_method = md_subpel_ctrls.subpel_search_method ==
            SUBPEL_TREE
        ? svt_av1_find_best_sub_pixel_tree
        : svt_av1_find_best_sub_pixel_tree_pruned;

    int besterr = subpel_search_method(xd,
                                       (const struct AV1Common *const)cm,
                                       ms_params,
                                       subpel_start_mv,
                                       &best_mv.as_mv,
                                       &not_used,
                                       &pred_sse,
                                       NULL);
#else
    int besterr = svt_av1_find_best_sub_pixel_tree(xd,
                                                   (const struct AV1Common *const)cm,
                                                   ms_params,
                                                   subpel_start_mv,
                                                   &best_mv.as_mv,
                                                   &not_used,
                                                   &pred_sse,
                                                   NULL);
#endif
    *me_mv_x = best_mv.as_mv.col;
    *me_mv_y = best_mv.as_mv.row;

    return besterr;
}
// Copy ME_MVs (generated @ PA) from input buffer (pcs_ptr-> .. ->me_results) to local
// MD buffers (context_ptr->sb_me_mv)
void read_refine_me_mvs(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                        EbPictureBufferDesc *input_picture_ptr) {
    const SequenceControlSet *scs_ptr = (SequenceControlSet *)pcs_ptr->scs_wrapper_ptr->object_ptr;

    derive_me_offsets(scs_ptr, pcs_ptr, context_ptr);
    uint8_t hbd_mode_decision = context_ptr->hbd_mode_decision == EB_DUAL_BIT_MD
        ? EB_8_BIT_MD
        : context_ptr->hbd_mode_decision;
    input_picture_ptr         = hbd_mode_decision ? pcs_ptr->input_frame16bit
                                                  : pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr;

    //Update input origin
    uint32_t input_origin_index = (context_ptr->blk_origin_y + input_picture_ptr->origin_y) *
            input_picture_ptr->stride_y +
        (context_ptr->blk_origin_x + input_picture_ptr->origin_x);
    // Get parent_depth_idx_mds
    uint16_t parent_depth_idx_mds = 0;
    if (context_ptr->blk_geom->sq_size <
        ((scs_ptr->seq_header.sb_size == BLOCK_128X128) ? 128 : 64))
        //Set parent to be considered
        parent_depth_idx_mds = (context_ptr->blk_geom->sqi_mds -
                                (context_ptr->blk_geom->quadi - 3) *
                                    ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128]
                                                   [context_ptr->blk_geom->depth]) -
            parent_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128]
                               [context_ptr->blk_geom->depth];
#if FTR_PD2_REDUCE_MDS0
    context_ptr->md_me_dist = (uint32_t)~0;
#endif
    for (uint32_t ref_it = 0; ref_it < pcs_ptr->parent_pcs_ptr->tot_ref_frame_types; ++ref_it) {
        MvReferenceFrame ref_pair = pcs_ptr->parent_pcs_ptr->ref_frame_type_arr[ref_it];

        MvReferenceFrame rf[2];
        av1_set_ref_frame(rf, ref_pair);

        if (rf[1] == NONE_FRAME) {
            uint8_t            list_idx = get_list_idx(rf[0]);
            uint8_t            ref_idx  = get_ref_frame_idx(rf[0]);
            EbReferenceObject *ref_obj  = pcs_ptr->ref_pic_ptr_array[list_idx][ref_idx]->object_ptr;
            EbPictureBufferDesc *ref_pic = hbd_mode_decision ? ref_obj->reference_picture16bit
                                                             : ref_obj->reference_picture;
            // Get the ME MV
            const MeSbResults *me_results =
                pcs_ptr->parent_pcs_ptr->pa_me_data->me_results[context_ptr->me_sb_addr];
#if REDUCE_PME_SEARCH || FTR_PD2_REDUCE_MDS0
            context_ptr->md_me_cost[list_idx][ref_idx] = (uint32_t)~0;
#endif
            if (is_me_data_present(context_ptr, me_results, list_idx, ref_idx)) {
                int16_t me_mv_x;
                int16_t me_mv_y;
                if (context_ptr->avail_blk_flag[context_ptr->blk_geom->sqi_mds] &&
                    // If NSQ then use the MV of SQ as default MV center
                    (context_ptr->blk_geom->bwidth != context_ptr->blk_geom->bheight) &&
                    // Not applicable for BLOCK_128X64 and BLOCK_64X128 as the 2nd part of each and BLOCK_128X128 do not share the same me_results
                    context_ptr->blk_geom->bsize != BLOCK_64X128 &&
                    context_ptr->blk_geom->bsize != BLOCK_128X64) {
                    me_mv_x = (context_ptr->sb_me_mv[context_ptr->blk_geom->sqi_mds][list_idx]
                                                    [ref_idx][0] +
                               4) &
                        ~0x07;
                    me_mv_y = (context_ptr->sb_me_mv[context_ptr->blk_geom->sqi_mds][list_idx]
                                                    [ref_idx][1] +
                               4) &
                        ~0x07;

                    clip_mv_on_pic_boundary(context_ptr->blk_origin_x,
                                            context_ptr->blk_origin_y,
                                            context_ptr->blk_geom->bwidth,
                                            context_ptr->blk_geom->bheight,
                                            ref_pic,
                                            &me_mv_x,
                                            &me_mv_y);

                } else if (context_ptr->blk_geom->bsize == BLOCK_4X4 &&
                           context_ptr->avail_blk_flag[parent_depth_idx_mds]) {
                    me_mv_x = (context_ptr->sb_me_mv[parent_depth_idx_mds][list_idx][ref_idx][0] +
                               4) &
                        ~0x07;
                    me_mv_y = (context_ptr->sb_me_mv[parent_depth_idx_mds][list_idx][ref_idx][1] +
                               4) &
                        ~0x07;

                    clip_mv_on_pic_boundary(context_ptr->blk_origin_x,
                                            context_ptr->blk_origin_y,
                                            context_ptr->blk_geom->bwidth,
                                            context_ptr->blk_geom->bheight,
                                            ref_pic,
                                            &me_mv_x,
                                            &me_mv_y);

                } else {
                    if (list_idx == 0) {
                        me_mv_x = (me_results
                                       ->me_mv_array[context_ptr->me_block_offset * MAX_PA_ME_MV +
                                                     ref_idx]
                                       .x_mv)
                            << 1;
                        me_mv_y = (me_results
                                       ->me_mv_array[context_ptr->me_block_offset * MAX_PA_ME_MV +
                                                     ref_idx]
                                       .y_mv)
                            << 1;
                    } else {
                        me_mv_x = (me_results
                                       ->me_mv_array[context_ptr->me_block_offset * MAX_PA_ME_MV +
                                                     4 + ref_idx]
                                       .x_mv)
                            << 1;
                        me_mv_y = (me_results
                                       ->me_mv_array[context_ptr->me_block_offset * MAX_PA_ME_MV +
                                                     4 + ref_idx]
                                       .y_mv)
                            << 1;
                    }
                }
                // Set ref MV
                context_ptr->ref_mv.col = context_ptr->mvp_array[list_idx][ref_idx][0].col;
                context_ptr->ref_mv.row = context_ptr->mvp_array[list_idx][ref_idx][0].row;
                if ((context_ptr->blk_geom->bwidth != context_ptr->blk_geom->bheight) &&
                    context_ptr->md_nsq_motion_search_ctrls.enabled) {
                    md_nsq_motion_search(pcs_ptr,
                                         context_ptr,
                                         input_picture_ptr,
                                         input_origin_index,
                                         list_idx,
                                         ref_idx,
                                         me_results,
                                         &me_mv_x,
                                         &me_mv_y);
                } else if (context_ptr->md_sq_me_ctrls.enabled) {
                    md_sq_motion_search(pcs_ptr,
                                        context_ptr,
                                        input_picture_ptr,
                                        input_origin_index,
                                        list_idx,
                                        ref_idx,
                                        &me_mv_x,
                                        &me_mv_y);
                }
                context_ptr->post_subpel_me_mv_cost[list_idx][ref_idx] = (int32_t)~0;
                context_ptr->fp_me_mv[list_idx][ref_idx].col           = me_mv_x;
                context_ptr->fp_me_mv[list_idx][ref_idx].row           = me_mv_y;
                context_ptr->sub_me_mv[list_idx][ref_idx].col          = me_mv_x;
                context_ptr->sub_me_mv[list_idx][ref_idx].row          = me_mv_y;
                if (context_ptr->md_subpel_me_ctrls.enabled) {
                    // Copy ME MV before subpel
                    context_ptr->fp_me_mv[list_idx][ref_idx].col           = me_mv_x;
                    context_ptr->fp_me_mv[list_idx][ref_idx].row           = me_mv_y;
                    context_ptr->post_subpel_me_mv_cost[list_idx][ref_idx] = (uint32_t)
                        md_subpel_search(
                            pcs_ptr,
                            context_ptr,
                            context_ptr->md_subpel_me_ctrls,
                            pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr, // 10BIT not supported
                            list_idx,
                            ref_idx,
                            &me_mv_x,
                            &me_mv_y);
                    // Copy ME MV after subpel
                    context_ptr->sub_me_mv[list_idx][ref_idx].col = me_mv_x;
                    context_ptr->sub_me_mv[list_idx][ref_idx].row = me_mv_y;
#if REDUCE_PME_SEARCH || FTR_PD2_REDUCE_MDS0
                    context_ptr->md_me_cost[list_idx][ref_idx] =
                        context_ptr->post_subpel_me_mv_cost[list_idx][ref_idx];
#endif
                }
#if FTR_PD2_REDUCE_MDS0
                if (context_ptr->md_me_cost[list_idx][ref_idx] < context_ptr->md_me_dist)
                    context_ptr->md_me_dist = context_ptr->md_me_cost[list_idx][ref_idx];
#endif
                context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds][list_idx][ref_idx][0] =
                    me_mv_x;
                context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds][list_idx][ref_idx][1] =
                    me_mv_y;
                clip_mv_on_pic_boundary(
                    context_ptr->blk_origin_x,
                    context_ptr->blk_origin_y,
                    context_ptr->blk_geom->bwidth,
                    context_ptr->blk_geom->bheight,
                    ref_pic,
                    &context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds][list_idx][ref_idx][0],
                    &context_ptr
                         ->sb_me_mv[context_ptr->blk_geom->blkidx_mds][list_idx][ref_idx][1]);
            }
        }
    }
}
void perform_md_reference_pruning(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                                  EbPictureBufferDesc *input_picture_ptr) {
    uint32_t early_inter_distortion_array[MAX_NUM_OF_REF_PIC_LIST * REF_LIST_MAX_DEPTH];
#if FTR_NEW_REF_PRUNING_CTRLS
    uint32_t dev_to_the_best[MAX_NUM_OF_REF_PIC_LIST * REF_LIST_MAX_DEPTH] = {0};
#endif
    // Reset ref_filtering_res
    for (uint32_t gi = 0; gi < TOT_INTER_GROUP; gi++)
        for (uint32_t li = 0; li < MAX_NUM_OF_REF_PIC_LIST; li++)
            for (uint32_t ri = 0; ri < REF_LIST_MAX_DEPTH; ri++) {
                context_ptr->ref_filtering_res[gi][li][ri].list_i    = li;
                context_ptr->ref_filtering_res[gi][li][ri].ref_i     = ri;
                context_ptr->ref_filtering_res[gi][li][ri].dist      = (uint32_t)~0;
                context_ptr->ref_filtering_res[gi][li][ri].do_ref    = 1;
                context_ptr->ref_filtering_res[gi][li][ri].valid_ref = EB_FALSE;
            }

    for (uint32_t li = 0; li < MAX_NUM_OF_REF_PIC_LIST; li++)
        for (uint32_t ri = 0; ri < REF_LIST_MAX_DEPTH; ri++)
            early_inter_distortion_array[li * REF_LIST_MAX_DEPTH + ri] = (uint32_t)~0;
    uint8_t hbd_mode_decision = context_ptr->hbd_mode_decision == EB_DUAL_BIT_MD
        ? EB_8_BIT_MD
        : context_ptr->hbd_mode_decision;

    input_picture_ptr = hbd_mode_decision ? pcs_ptr->input_frame16bit
                                          : pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr;

    // Update input origin
    uint32_t input_origin_index = (context_ptr->blk_origin_y + input_picture_ptr->origin_y) *
            input_picture_ptr->stride_y +
        (context_ptr->blk_origin_x + input_picture_ptr->origin_x);
    for (uint32_t ref_it = 0; ref_it < pcs_ptr->parent_pcs_ptr->tot_ref_frame_types; ++ref_it) {
        MvReferenceFrame ref_pair = pcs_ptr->parent_pcs_ptr->ref_frame_type_arr[ref_it];
        MvReferenceFrame rf[2];
        av1_set_ref_frame(rf, ref_pair);

        if (rf[1] == NONE_FRAME) {
            uint32_t best_mvp_distortion = (int32_t)~0;
            uint8_t  list_idx            = get_list_idx(rf[0]);
            uint8_t  ref_idx             = get_ref_frame_idx(rf[0]);
            // Step 1: derive the best MVP in term of distortion
            for (int8_t mvp_index = 0; mvp_index < context_ptr->mvp_count[list_idx][ref_idx];
                 mvp_index++) {
                // MVP Distortion
                EbReferenceObject *ref_obj =
                    pcs_ptr->ref_pic_ptr_array[list_idx][ref_idx]->object_ptr;
                EbPictureBufferDesc *ref_pic = hbd_mode_decision ? ref_obj->reference_picture16bit
                                                                 : ref_obj->reference_picture;
                clip_mv_on_pic_boundary(context_ptr->blk_origin_x,
                                        context_ptr->blk_origin_y,
                                        context_ptr->blk_geom->bwidth,
                                        context_ptr->blk_geom->bheight,
                                        ref_pic,
                                        &context_ptr->mvp_array[list_idx][ref_idx][mvp_index].col,
                                        &context_ptr->mvp_array[list_idx][ref_idx][mvp_index].row);
                // Never be negative here
                int32_t ref_origin_index = ref_pic->origin_x +
                    (context_ptr->blk_origin_x +
                     (context_ptr->mvp_array[list_idx][ref_idx][mvp_index].col >> 3)) +
                    (context_ptr->blk_origin_y +
                     (context_ptr->mvp_array[list_idx][ref_idx][mvp_index].row >> 3) +
                     ref_pic->origin_y) *
                        ref_pic->stride_y;
                assert((context_ptr->blk_geom->bwidth >> 3) < 17);
                uint32_t mvp_distortion = hbd_mode_decision
                    ? sad_16b_kernel(((uint16_t *)input_picture_ptr->buffer_y) + input_origin_index,
                                     input_picture_ptr->stride_y,
                                     ((uint16_t *)ref_pic->buffer_y) + ref_origin_index,
                                     ref_pic->stride_y,
                                     context_ptr->blk_geom->bheight,
                                     context_ptr->blk_geom->bwidth)
                    : svt_nxm_sad_kernel_sub_sampled(
                          input_picture_ptr->buffer_y + input_origin_index,
                          input_picture_ptr->stride_y,
                          ref_pic->buffer_y + ref_origin_index,
                          ref_pic->stride_y,
                          context_ptr->blk_geom->bheight,
                          context_ptr->blk_geom->bwidth);

                if (mvp_distortion < best_mvp_distortion)
                    best_mvp_distortion = mvp_distortion;
            }

            // Evaluate the PA_ME MVs (if available)
            const MeSbResults *me_results =
                pcs_ptr->parent_pcs_ptr->pa_me_data->me_results[context_ptr->me_sb_addr];
            uint32_t pa_me_distortion = (uint32_t)~0; //any non zero value
            if (is_me_data_present(context_ptr, me_results, list_idx, ref_idx)) {
                int16_t me_mv_x;
                int16_t me_mv_y;
                if (list_idx == 0) {
                    me_mv_x = context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds][REF_LIST_0]
                                                   [ref_idx][0];
                    me_mv_y = context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds][REF_LIST_0]
                                                   [ref_idx][1];
                } else {
                    me_mv_x = context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds][REF_LIST_1]
                                                   [ref_idx][0];
                    me_mv_y = context_ptr->sb_me_mv[context_ptr->blk_geom->blkidx_mds][REF_LIST_1]
                                                   [ref_idx][1];
                }
                // Round-up to the closest integer the ME MV
                me_mv_x = (me_mv_x + 4) & ~0x07;
                me_mv_y = (me_mv_y + 4) & ~0x07;
                EbReferenceObject *ref_obj =
                    pcs_ptr->ref_pic_ptr_array[list_idx][ref_idx]->object_ptr;
                EbPictureBufferDesc *ref_pic = hbd_mode_decision ? ref_obj->reference_picture16bit
                                                                 : ref_obj->reference_picture;
                clip_mv_on_pic_boundary(context_ptr->blk_origin_x,
                                        context_ptr->blk_origin_y,
                                        context_ptr->blk_geom->bwidth,
                                        context_ptr->blk_geom->bheight,
                                        ref_pic,
                                        &me_mv_x,
                                        &me_mv_y);
                // Never be negative here
                int32_t ref_origin_index = ref_pic->origin_x +
                    (context_ptr->blk_origin_x + (me_mv_x >> 3)) +
                    (context_ptr->blk_origin_y + (me_mv_y >> 3) + ref_pic->origin_y) *
                        ref_pic->stride_y;
                assert((context_ptr->blk_geom->bwidth >> 3) < 17);
                pa_me_distortion = hbd_mode_decision
                    ? sad_16b_kernel(((uint16_t *)input_picture_ptr->buffer_y) + input_origin_index,
                                     input_picture_ptr->stride_y,
                                     ((uint16_t *)ref_pic->buffer_y) + ref_origin_index,
                                     ref_pic->stride_y,
                                     context_ptr->blk_geom->bheight,
                                     context_ptr->blk_geom->bwidth)
                    : svt_nxm_sad_kernel_sub_sampled(
                          input_picture_ptr->buffer_y + input_origin_index,
                          input_picture_ptr->stride_y,
                          ref_pic->buffer_y + ref_origin_index,
                          ref_pic->stride_y,
                          context_ptr->blk_geom->bheight,
                          context_ptr->blk_geom->bwidth);
            }

            // early_inter_distortion_array
            for (uint32_t gi = 0; gi < TOT_INTER_GROUP; gi++) {
                context_ptr->ref_filtering_res[gi][list_idx][ref_idx].valid_ref = EB_TRUE;
                context_ptr->ref_filtering_res[gi][list_idx][ref_idx].dist      = MIN(
                    pa_me_distortion, best_mvp_distortion);
            }
            early_inter_distortion_array[list_idx * REF_LIST_MAX_DEPTH + ref_idx] = MIN(
                pa_me_distortion, best_mvp_distortion);
        }
    }

    // Sort early_inter_distortion_array
    unsigned num_of_cand_to_sort = MAX_NUM_OF_REF_PIC_LIST * REF_LIST_MAX_DEPTH;
#if FTR_NEW_REF_PRUNING_CTRLS
    uint32_t min_dist = (uint32_t)~0;

    for (unsigned i = 0; i < num_of_cand_to_sort - 1; ++i)
        min_dist = MIN(min_dist, early_inter_distortion_array[i]);

    for (unsigned i = 0; i < num_of_cand_to_sort - 1; ++i)
        dev_to_the_best[i] = ((MAX(early_inter_distortion_array[i], 1) - MAX(min_dist, 1)) * 100) /
            MAX(min_dist, 1);
#else
    for (unsigned i = 0; i < num_of_cand_to_sort - 1; ++i)
        for (unsigned j = i + 1; j < num_of_cand_to_sort; ++j)
            if (early_inter_distortion_array[j] < early_inter_distortion_array[i]) {
                uint32_t temp = early_inter_distortion_array[i];
                early_inter_distortion_array[i] = early_inter_distortion_array[j];
                early_inter_distortion_array[j] = temp;
            }
#endif
    for (unsigned gi = 0; gi < TOT_INTER_GROUP; gi++) {
#if !FTR_NEW_REF_PRUNING_CTRLS
        uint8_t best_refs        = context_ptr->ref_pruning_ctrls.best_refs[gi];
        uint8_t total_tagged_ref = 0;
#endif
        for (unsigned li = 0; li < MAX_NUM_OF_REF_PIC_LIST; li++)
            for (unsigned ri = 0; ri < REF_LIST_MAX_DEPTH; ri++)
                if (context_ptr->ref_filtering_res[gi][li][ri].valid_ref) {
                    context_ptr->ref_filtering_res[gi][li][ri].do_ref = 0;
#if FTR_NEW_REF_PRUNING_CTRLS
                    uint32_t offset = (ri <= 1) ? 0
                        : (ri <= 2)             ? context_ptr->ref_pruning_ctrls.ref_idx_2_offset
                                                : context_ptr->ref_pruning_ctrls.ref_idx_3_offset;

                    uint32_t pruning_th = (offset == (uint32_t)~0) ? 0
                        : (context_ptr->ref_pruning_ctrls.max_dev_to_best[gi] == (uint32_t)~0)
                        ? (uint32_t)~0
                        : MAX(0,
                              ((int64_t)context_ptr->ref_pruning_ctrls.max_dev_to_best[gi] -
                               (int64_t)offset));

                    if (dev_to_the_best[li * REF_LIST_MAX_DEPTH + ri] < pruning_th) {
#else
                    if (context_ptr->ref_filtering_res[gi][li][ri].dist <=
                            early_inter_distortion_array[(uint8_t)(best_refs - 1)] &&
                        total_tagged_ref < best_refs) {
                        total_tagged_ref++;
#endif
                        context_ptr->ref_filtering_res[gi][li][ri].do_ref = 1;
                    }
                }
    }
}
/*
 * Read/store all nearest/near MVs for a block for single ref case, and save the best distortion for each ref.
 */
void build_single_ref_mvp_array(PictureControlSet *pcs, ModeDecisionContext *ctx) {
    for (uint32_t ref_it = 0; ref_it < pcs->parent_pcs_ptr->tot_ref_frame_types; ++ref_it) {
        MvReferenceFrame ref_pair = pcs->parent_pcs_ptr->ref_frame_type_arr[ref_it];

        MacroBlockD *xd = ctx->blk_ptr->av1xd;
        IntMv        nearestmv[2], nearmv[2], ref_mv[2];

        MvReferenceFrame rf[2];
        av1_set_ref_frame(rf, ref_pair);
        // Single ref
        if (rf[1] == NONE_FRAME) {
            MvReferenceFrame frame_type = rf[0];
            uint8_t          list_idx   = get_list_idx(rf[0]);
            uint8_t          ref_idx    = get_ref_frame_idx(rf[0]);
            if (ctx->shut_fast_rate) {
                ctx->mvp_array[list_idx][ref_idx][0].col = 0;
                ctx->mvp_array[list_idx][ref_idx][0].row = 0;
                ctx->mvp_count[list_idx][ref_idx]        = 1;
                continue;
            }
            uint8_t drli, max_drl_index;
            int8_t  mvp_count = 0;

            //NEAREST
            ctx->mvp_array[list_idx][ref_idx][mvp_count].col =
                (ctx->md_local_blk_unit[ctx->blk_geom->blkidx_mds]
                     .ref_mvs[frame_type][0]
                     .as_mv.col +
                 4) &
                ~0x07;
            ctx->mvp_array[list_idx][ref_idx][mvp_count].row =
                (ctx->md_local_blk_unit[ctx->blk_geom->blkidx_mds]
                     .ref_mvs[frame_type][0]
                     .as_mv.row +
                 4) &
                ~0x07;
            mvp_count++;

            //NEAR
            max_drl_index = get_max_drl_index(xd->ref_mv_count[frame_type], NEARMV);

            for (drli = 0; drli < max_drl_index; drli++) {
                get_av1_mv_pred_drl(
                    ctx, ctx->blk_ptr, frame_type, 0, NEARMV, drli, nearestmv, nearmv, ref_mv);

                if (((nearmv[0].as_mv.col + 4) & ~0x07) !=
                        ctx->mvp_array[list_idx][ref_idx][0].col &&
                    ((nearmv[0].as_mv.row + 4) & ~0x07) !=
                        ctx->mvp_array[list_idx][ref_idx][0].row) {
                    ctx->mvp_array[list_idx][ref_idx][mvp_count].col = (nearmv[0].as_mv.col + 4) &
                        ~0x07;
                    ctx->mvp_array[list_idx][ref_idx][mvp_count].row = (nearmv[0].as_mv.row + 4) &
                        ~0x07;
                    mvp_count++;
                }
            }
            ctx->mvp_count[list_idx][ref_idx] = mvp_count;
        }
    }
}
EbBool is_valid_unipred_ref(struct ModeDecisionContext *context_ptr, uint8_t inter_cand_group,
                            uint8_t list_idx, uint8_t ref_idx);
void   pme_search(PictureControlSet *pcs, ModeDecisionContext *ctx,
                  EbPictureBufferDesc *input_picture_ptr) {
#if !CLN_INIT_OP
    memset(ctx->valid_pme_mv, 0, MAX_NUM_OF_REF_PIC_LIST * REF_LIST_MAX_DEPTH);
#endif
    uint8_t hbd_mode_decision = ctx->hbd_mode_decision == EB_DUAL_BIT_MD ? EB_8_BIT_MD
                                                                         : ctx->hbd_mode_decision;

    input_picture_ptr = hbd_mode_decision ? pcs->input_frame16bit
                                          : pcs->parent_pcs_ptr->enhanced_picture_ptr;

    uint32_t input_origin_index = (ctx->blk_origin_y + input_picture_ptr->origin_y) *
            input_picture_ptr->stride_y +
        (ctx->blk_origin_x + input_picture_ptr->origin_x);

    for (uint32_t ref_it = 0; ref_it < pcs->parent_pcs_ptr->tot_ref_frame_types; ++ref_it) {
        MvReferenceFrame ref_pair = pcs->parent_pcs_ptr->ref_frame_type_arr[ref_it];
        MvReferenceFrame rf[2];
        av1_set_ref_frame(rf, ref_pair);

        // Reset search variable(s)
        uint32_t best_mvp_cost           = (int32_t)~0;
        int16_t  best_search_mvx         = (int16_t)~0;
        int16_t  best_search_mvy         = (int16_t)~0;
        uint32_t pme_mv_cost             = (int32_t)~0;
        uint32_t me_mv_cost              = (int32_t)~0;
        uint32_t post_subpel_pme_mv_cost = (int32_t)~0;

        if (rf[1] == NONE_FRAME) {
            uint8_t list_idx = get_list_idx(rf[0]);
            uint8_t ref_idx  = get_ref_frame_idx(rf[0]);
#if CLN_INIT_OP
            ctx->valid_pme_mv[list_idx][ref_idx] = 0;
#endif
            EbReferenceObject *  ref_obj = pcs->ref_pic_ptr_array[list_idx][ref_idx]->object_ptr;
            EbPictureBufferDesc *ref_pic = hbd_mode_decision ? ref_obj->reference_picture16bit
                                                             : ref_obj->reference_picture;

            // -------
            // Use scaled references if resolution of the reference is different from that of the input
            // -------
            use_scaled_rec_refs_if_needed(pcs, input_picture_ptr, ref_obj, &ref_pic);

            if (!is_valid_unipred_ref(ctx, PRED_ME_GROUP, list_idx, ref_idx))
                continue;
#if !FTR_NEW_REF_PRUNING_CTRLS
            if (ref_idx > ctx->md_max_ref_count - 1)
                continue;
#endif
            // Get the ME MV
            const MeSbResults *me_results =
                pcs->parent_pcs_ptr->pa_me_data->me_results[ctx->me_sb_addr];

            uint8_t me_data_present = is_me_data_present(ctx, me_results, list_idx, ref_idx);

            if (me_data_present) {
                int16_t me_mv_x;
                int16_t me_mv_y;
                if (list_idx == 0) {
                    me_mv_x = ctx->sb_me_mv[ctx->blk_geom->blkidx_mds][REF_LIST_0][ref_idx][0];
                    me_mv_y = ctx->sb_me_mv[ctx->blk_geom->blkidx_mds][REF_LIST_0][ref_idx][1];
                } else {
                    me_mv_x = ctx->sb_me_mv[ctx->blk_geom->blkidx_mds][REF_LIST_1][ref_idx][0];
                    me_mv_y = ctx->sb_me_mv[ctx->blk_geom->blkidx_mds][REF_LIST_1][ref_idx][1];
                }
                // Round-up to the closest integer the ME MV
                me_mv_x = (me_mv_x + 4) & ~0x07;
                me_mv_y = (me_mv_y + 4) & ~0x07;

                // Set a ref MV (nearest) for the ME MV
                ctx->ref_mv.col = ctx->mvp_array[list_idx][ref_idx][0].col;
                ctx->ref_mv.row = ctx->mvp_array[list_idx][ref_idx][0].row;
                md_full_pel_search(pcs,
                                   ctx,
                                   input_picture_ptr,
                                   ref_pic,
                                   input_origin_index,
                                   ctx->md_pme_ctrls.use_ssd,
                                   me_mv_x,
                                   me_mv_y,
                                   0,
                                   0,
                                   0,
                                   0,
                                   1,
                                   &me_mv_x,
                                   &me_mv_y,
                                   &me_mv_cost);
            }

            // Step 1: derive the best MVP in term of distortion
            int16_t best_mvp_x = 0;
            int16_t best_mvp_y = 0;

            for (int8_t mvp_index = 0; mvp_index < ctx->mvp_count[list_idx][ref_idx]; mvp_index++) {
                // Set a ref MV (MVP under eval) for the MVP under eval
                ctx->ref_mv.col = ctx->mvp_array[list_idx][ref_idx][mvp_index].col;
                ctx->ref_mv.row = ctx->mvp_array[list_idx][ref_idx][mvp_index].row;

                clip_mv_on_pic_boundary(ctx->blk_origin_x,
                                        ctx->blk_origin_y,
                                        ctx->blk_geom->bwidth,
                                        ctx->blk_geom->bheight,
                                        ref_pic,
                                        &ctx->mvp_array[list_idx][ref_idx][mvp_index].col,
                                        &ctx->mvp_array[list_idx][ref_idx][mvp_index].row);

                md_full_pel_search(pcs,
                                   ctx,
                                   input_picture_ptr,
                                   ref_pic,
                                   input_origin_index,
                                   ctx->md_pme_ctrls.use_ssd,
                                   ctx->mvp_array[list_idx][ref_idx][mvp_index].col,
                                   ctx->mvp_array[list_idx][ref_idx][mvp_index].row,
                                   0,
                                   0,
                                   0,
                                   0,
                                   1,
                                   &best_mvp_x,
                                   &best_mvp_y,
                                   &best_mvp_cost);
            }

            uint8_t skip_search = 0;
            if (me_data_present) {
                int64_t pme_to_me_cost_dev =
                    (((int64_t)MAX(best_mvp_cost, 1) - (int64_t)MAX(me_mv_cost, 1)) * 100) /
                    (int64_t)MAX(me_mv_cost, 1);

                if ((ABS(ctx->fp_me_mv[list_idx][ref_idx].col - best_mvp_x) <=
                         ctx->md_pme_ctrls.pre_fp_pme_to_me_mv_th &&
                     ABS(ctx->fp_me_mv[list_idx][ref_idx].row - best_mvp_y) <=
                         ctx->md_pme_ctrls.pre_fp_pme_to_me_mv_th) ||
                    pme_to_me_cost_dev >= ctx->md_pme_ctrls.pre_fp_pme_to_me_cost_th) {
                    best_search_mvx = ctx->sub_me_mv[list_idx][ref_idx].col;
                    best_search_mvy = ctx->sub_me_mv[list_idx][ref_idx].row;
                    skip_search     = 1;
                }
            }
            if (!skip_search) {
                // Set ref MV
                ctx->ref_mv.col = best_mvp_x;
                ctx->ref_mv.row = best_mvp_y;

                md_full_pel_search(pcs,
                                   ctx,
                                   input_picture_ptr,
                                   ref_pic,
                                   input_origin_index,
                                   ctx->md_pme_ctrls.use_ssd,
                                   best_mvp_x,
                                   best_mvp_y,
                                   -(ctx->md_pme_ctrls.full_pel_search_width >> 1),
                                   +(ctx->md_pme_ctrls.full_pel_search_width >> 1),
                                   -(ctx->md_pme_ctrls.full_pel_search_height >> 1),
                                   +(ctx->md_pme_ctrls.full_pel_search_height >> 1),
                                   1,
                                   &best_search_mvx,
                                   &best_search_mvy,
                                   &pme_mv_cost);
            }

            uint8_t skip_subpel_search = 0;
            if (me_data_present) {
                int64_t pme_to_me_cost_dev =
                    (((int64_t)MAX(pme_mv_cost, 1) - (int64_t)MAX(me_mv_cost, 1)) * 100) /
                    (int64_t)MAX(me_mv_cost, 1);

                if ((ABS(ctx->fp_me_mv[list_idx][ref_idx].col - best_search_mvx) <=
                         ctx->md_pme_ctrls.post_fp_pme_to_me_mv_th &&
                     ABS(ctx->fp_me_mv[list_idx][ref_idx].row - best_search_mvy) <=
                         ctx->md_pme_ctrls.post_fp_pme_to_me_mv_th) ||
                    pme_to_me_cost_dev >= ctx->md_pme_ctrls.post_fp_pme_to_me_cost_th) {
                    best_search_mvx    = ctx->sub_me_mv[list_idx][ref_idx].col;
                    best_search_mvy    = ctx->sub_me_mv[list_idx][ref_idx].row;
                    skip_subpel_search = 1;
                }
            }
            if (ctx->md_subpel_pme_ctrls.enabled && !skip_subpel_search) {
                post_subpel_pme_mv_cost = (uint32_t)md_subpel_search(
                    pcs,
                    ctx,
                    ctx->md_subpel_pme_ctrls,
                    pcs->parent_pcs_ptr->enhanced_picture_ptr, // 10BIT not supported
                    list_idx,
                    ref_idx,
                    &best_search_mvx,
                    &best_search_mvy);
            }

            //check if final MV is within AV1 limits
            check_mv_validity(best_search_mvx, best_search_mvy, 0);

            ctx->best_pme_mv[list_idx][ref_idx][0] = best_search_mvx;
            ctx->best_pme_mv[list_idx][ref_idx][1] = best_search_mvy;
            ctx->valid_pme_mv[list_idx][ref_idx]   = 1;
            ctx->pme_res[list_idx][ref_idx].dist   = (skip_search || skip_subpel_search)
                  ? ctx->post_subpel_me_mv_cost[list_idx][ref_idx]
                  : post_subpel_pme_mv_cost;
        }
    }
#if !OPT_PME_RES_PREP
#if CLN_INIT_OP
    if (ctx->obmc_ctrls.enabled) {
#endif
        uint32_t    num_of_cand_to_sort = MAX_NUM_OF_REF_PIC_LIST * REF_LIST_MAX_DEPTH;
        RefResults *res_p               = ctx->pme_res[0];
        for (uint32_t i = 0; i < num_of_cand_to_sort - 1; ++i) {
            for (uint32_t j = i + 1; j < num_of_cand_to_sort; ++j) {
                if (res_p[j].dist < res_p[i].dist) {
                    RefResults temp = res_p[i];
                    res_p[i]        = res_p[j];
                    res_p[j]        = temp;
                }
            }
        }
#if CLN_INIT_OP
    }
#endif
#endif
}
void av1_cost_calc_cfl(PictureControlSet *pcs_ptr, ModeDecisionCandidateBuffer *candidate_buffer,
                       SuperBlock *sb_ptr, ModeDecisionContext *context_ptr,
                       uint32_t component_mask, EbPictureBufferDesc *input_picture_ptr,
                       uint32_t input_cb_origin_in_index, uint32_t blk_chroma_origin_index,
                       uint64_t full_distortion[DIST_CALC_TOTAL], uint64_t *coeff_bits,
                       EbBool check_dc) {
    ModeDecisionCandidate *candidate_ptr = candidate_buffer->candidate_ptr;
    uint32_t               count_non_zero_coeffs[3][MAX_NUM_OF_TU_PER_CU];
    uint64_t               cb_full_distortion[DIST_CALC_TOTAL];
    uint64_t               cr_full_distortion[DIST_CALC_TOTAL];
    uint32_t               chroma_width  = context_ptr->blk_geom->bwidth_uv;
    uint32_t               chroma_height = context_ptr->blk_geom->bheight_uv;
    // FullLoop and TU search
    uint16_t cb_qindex = context_ptr->qp_index;
    uint16_t cr_qindex = cb_qindex;

    full_distortion[DIST_CALC_RESIDUAL]   = 0;
    full_distortion[DIST_CALC_PREDICTION] = 0;
    *coeff_bits                           = 0;

    // Loop over alphas and find the best
    if (component_mask == COMPONENT_CHROMA_CB || component_mask == COMPONENT_CHROMA ||
        component_mask == COMPONENT_ALL) {
        cb_full_distortion[DIST_CALC_RESIDUAL]   = 0;
        cr_full_distortion[DIST_CALC_RESIDUAL]   = 0;
        cb_full_distortion[DIST_CALC_PREDICTION] = 0;
        cr_full_distortion[DIST_CALC_PREDICTION] = 0;
        uint64_t cb_coeff_bits                   = 0;
        uint64_t cr_coeff_bits                   = 0;
        int32_t  alpha_q3                        = (check_dc) ? 0
                                                              : cfl_idx_to_alpha(candidate_ptr->cfl_alpha_idx,
                                                         candidate_ptr->cfl_alpha_signs,
                                                         CFL_PRED_U); // once for U, once for V
        assert(chroma_width * CFL_BUF_LINE + chroma_height <= CFL_BUF_SQUARE);

        if (!context_ptr->hbd_mode_decision) {
            svt_cfl_predict_lbd(
                context_ptr->pred_buf_q3,
                &(candidate_buffer->prediction_ptr->buffer_cb[blk_chroma_origin_index]),
                candidate_buffer->prediction_ptr->stride_cb,
                &(context_ptr->cfl_temp_prediction_ptr->buffer_cb[blk_chroma_origin_index]),
                context_ptr->cfl_temp_prediction_ptr->stride_cb,
                alpha_q3,
                8,
                chroma_width,
                chroma_height);
        } else {
            svt_cfl_predict_hbd(
                context_ptr->pred_buf_q3,
                ((uint16_t *)candidate_buffer->prediction_ptr->buffer_cb) + blk_chroma_origin_index,
                candidate_buffer->prediction_ptr->stride_cb,
                ((uint16_t *)context_ptr->cfl_temp_prediction_ptr->buffer_cb) +
                    blk_chroma_origin_index,
                context_ptr->cfl_temp_prediction_ptr->stride_cb,
                alpha_q3,
                10,
                chroma_width,
                chroma_height);
        }

        // Cb Residual
        residual_kernel(input_picture_ptr->buffer_cb,
                        input_cb_origin_in_index,
                        input_picture_ptr->stride_cb,
                        context_ptr->cfl_temp_prediction_ptr->buffer_cb,
                        blk_chroma_origin_index,
                        context_ptr->cfl_temp_prediction_ptr->stride_cb,
                        (int16_t *)candidate_buffer->residual_ptr->buffer_cb,
                        blk_chroma_origin_index,
                        candidate_buffer->residual_ptr->stride_cb,
                        context_ptr->hbd_mode_decision,
                        chroma_width,
                        chroma_height);

        full_loop_r(sb_ptr,
                    candidate_buffer,
                    context_ptr,
                    input_picture_ptr,
                    pcs_ptr,
                    PICTURE_BUFFER_DESC_Cb_FLAG,
                    cb_qindex,
                    cr_qindex,
                    &(*count_non_zero_coeffs[1]),
                    &(*count_non_zero_coeffs[2]));

        // Create new function
        cu_full_distortion_fast_txb_mode_r(sb_ptr,
                                           candidate_buffer,
                                           context_ptr,
                                           candidate_ptr,
                                           pcs_ptr,
                                           input_picture_ptr,
                                           cb_full_distortion,
                                           cr_full_distortion,
                                           count_non_zero_coeffs,
                                           COMPONENT_CHROMA_CB,
                                           &cb_coeff_bits,
                                           &cr_coeff_bits,
                                           0);

        full_distortion[DIST_CALC_RESIDUAL] += cb_full_distortion[DIST_CALC_RESIDUAL];
        full_distortion[DIST_CALC_PREDICTION] += cb_full_distortion[DIST_CALC_PREDICTION];
        *coeff_bits += cb_coeff_bits;
    }
    if (component_mask == COMPONENT_CHROMA_CR || component_mask == COMPONENT_CHROMA ||
        component_mask == COMPONENT_ALL) {
        cb_full_distortion[DIST_CALC_RESIDUAL]   = 0;
        cr_full_distortion[DIST_CALC_RESIDUAL]   = 0;
        cb_full_distortion[DIST_CALC_PREDICTION] = 0;
        cr_full_distortion[DIST_CALC_PREDICTION] = 0;

        uint64_t cb_coeff_bits = 0;
        uint64_t cr_coeff_bits = 0;
        int32_t  alpha_q3      = check_dc ? 0
                                          : cfl_idx_to_alpha(candidate_ptr->cfl_alpha_idx,
                                                       candidate_ptr->cfl_alpha_signs,
                                                       CFL_PRED_V); // once for U, once for V
        assert(chroma_width * CFL_BUF_LINE + chroma_height <= CFL_BUF_SQUARE);

        if (!context_ptr->hbd_mode_decision) {
            svt_cfl_predict_lbd(
                context_ptr->pred_buf_q3,
                &(candidate_buffer->prediction_ptr->buffer_cr[blk_chroma_origin_index]),
                candidate_buffer->prediction_ptr->stride_cr,
                &(context_ptr->cfl_temp_prediction_ptr->buffer_cr[blk_chroma_origin_index]),
                context_ptr->cfl_temp_prediction_ptr->stride_cr,
                alpha_q3,
                8,
                chroma_width,
                chroma_height);
        } else {
            svt_cfl_predict_hbd(
                context_ptr->pred_buf_q3,
                ((uint16_t *)candidate_buffer->prediction_ptr->buffer_cr) + blk_chroma_origin_index,
                candidate_buffer->prediction_ptr->stride_cr,
                ((uint16_t *)context_ptr->cfl_temp_prediction_ptr->buffer_cr) +
                    blk_chroma_origin_index,
                context_ptr->cfl_temp_prediction_ptr->stride_cr,
                alpha_q3,
                10,
                chroma_width,
                chroma_height);
        }

        // Cr Residual
        residual_kernel(input_picture_ptr->buffer_cr,
                        input_cb_origin_in_index,
                        input_picture_ptr->stride_cr,
                        context_ptr->cfl_temp_prediction_ptr->buffer_cr,
                        blk_chroma_origin_index,
                        context_ptr->cfl_temp_prediction_ptr->stride_cr,
                        (int16_t *)candidate_buffer->residual_ptr->buffer_cr,
                        blk_chroma_origin_index,
                        candidate_buffer->residual_ptr->stride_cr,
                        context_ptr->hbd_mode_decision,
                        chroma_width,
                        chroma_height);

        full_loop_r(sb_ptr,
                    candidate_buffer,
                    context_ptr,
                    input_picture_ptr,
                    pcs_ptr,
                    PICTURE_BUFFER_DESC_Cr_FLAG,
                    cb_qindex,
                    cr_qindex,
                    &(*count_non_zero_coeffs[1]),
                    &(*count_non_zero_coeffs[2]));
        candidate_ptr->v_has_coeff = *count_non_zero_coeffs[2] ? EB_TRUE : EB_FALSE;

        // Create new function
        cu_full_distortion_fast_txb_mode_r(sb_ptr,
                                           candidate_buffer,
                                           context_ptr,
                                           candidate_ptr,
                                           pcs_ptr,
                                           input_picture_ptr,
                                           cb_full_distortion,
                                           cr_full_distortion,
                                           count_non_zero_coeffs,
                                           COMPONENT_CHROMA_CR,
                                           &cb_coeff_bits,
                                           &cr_coeff_bits,
                                           0);

        full_distortion[DIST_CALC_RESIDUAL] += cr_full_distortion[DIST_CALC_RESIDUAL];
        full_distortion[DIST_CALC_PREDICTION] += cr_full_distortion[DIST_CALC_PREDICTION];
        *coeff_bits += cr_coeff_bits;
    }
}

#define PLANE_SIGN_TO_JOINT_SIGN(plane, a, b) \
    (plane == CFL_PRED_U ? a * CFL_SIGNS + b - 1 : b * CFL_SIGNS + a - 1)
/*************************Pick the best alpha for cfl mode  or Choose DC******************************************************/
void md_cfl_rd_pick_alpha(PictureControlSet *pcs_ptr, ModeDecisionCandidateBuffer *candidate_buffer,
                          SuperBlock *sb_ptr, ModeDecisionContext *context_ptr,
                          EbPictureBufferDesc *input_picture_ptr, uint32_t input_cb_origin_in_index,
                          uint32_t blk_chroma_origin_index) {
    int64_t  best_rd = INT64_MAX;
    uint64_t full_distortion[DIST_CALC_TOTAL];
    uint64_t coeff_bits;

    uint32_t full_lambda = context_ptr->hbd_mode_decision
        ? context_ptr->full_lambda_md[EB_10_BIT_MD]
        : context_ptr->full_lambda_md[EB_8_BIT_MD];
#if CLN_FAST_COST
    const int64_t mode_rd = RDCOST(
        full_lambda,
        (uint64_t)context_ptr->md_rate_estimation_ptr
            ->intra_uv_mode_fac_bits[CFL_ALLOWED][candidate_buffer->candidate_ptr->intra_luma_mode]
                                    [UV_CFL_PRED],
        0);
#else
    const int64_t mode_rd = RDCOST(
        full_lambda,
        (uint64_t)candidate_buffer->candidate_ptr->md_rate_estimation_ptr
            ->intra_uv_mode_fac_bits[CFL_ALLOWED][candidate_buffer->candidate_ptr->intra_luma_mode]
                                    [UV_CFL_PRED],
        0);
#endif
    int64_t best_rd_uv[CFL_JOINT_SIGNS][CFL_PRED_PLANES];
    int32_t best_c[CFL_JOINT_SIGNS][CFL_PRED_PLANES];

    for (int32_t plane = 0; plane < CFL_PRED_PLANES; plane++) {
        coeff_bits                          = 0;
        full_distortion[DIST_CALC_RESIDUAL] = 0;
        for (int32_t joint_sign = 0; joint_sign < CFL_JOINT_SIGNS; joint_sign++) {
            best_rd_uv[joint_sign][plane] = INT64_MAX;
            best_c[joint_sign][plane]     = 0;
        }
        // Collect RD stats for an alpha value of zero in this plane.
        // Skip i == CFL_SIGN_ZERO as (0, 0) is invalid.
        for (int32_t i = CFL_SIGN_NEG; i < CFL_SIGNS; i++) {
            const int32_t joint_sign = PLANE_SIGN_TO_JOINT_SIGN(plane, CFL_SIGN_ZERO, i);
            if (i == CFL_SIGN_NEG) {
                candidate_buffer->candidate_ptr->cfl_alpha_idx   = 0;
                candidate_buffer->candidate_ptr->cfl_alpha_signs = joint_sign;

                av1_cost_calc_cfl(pcs_ptr,
                                  candidate_buffer,
                                  sb_ptr,
                                  context_ptr,
                                  (plane == 0) ? COMPONENT_CHROMA_CB : COMPONENT_CHROMA_CR,
                                  input_picture_ptr,
                                  input_cb_origin_in_index,
                                  blk_chroma_origin_index,
                                  full_distortion,
                                  &coeff_bits,
                                  0);

                if (coeff_bits == INT64_MAX)
                    break;
            }
#if CLN_FAST_COST
            const int32_t alpha_rate =
                context_ptr->md_rate_estimation_ptr->cfl_alpha_fac_bits[joint_sign][plane][0];
#else
            const int32_t alpha_rate = candidate_buffer->candidate_ptr->md_rate_estimation_ptr
                                           ->cfl_alpha_fac_bits[joint_sign][plane][0];
#endif
            best_rd_uv[joint_sign][plane] = RDCOST(
                full_lambda, coeff_bits + alpha_rate, full_distortion[DIST_CALC_RESIDUAL]);
        }
    }

    int32_t best_joint_sign = -1;

    for (int32_t plane = 0; plane < CFL_PRED_PLANES; plane++) {
        for (int32_t pn_sign = CFL_SIGN_NEG; pn_sign < CFL_SIGNS; pn_sign++) {
            int32_t progress = 0;
            for (int32_t c = 0; c < CFL_ALPHABET_SIZE; c++) {
                int32_t flag = 0;

                if (c > 2 && progress < c)
                    break;
                coeff_bits                          = 0;
                full_distortion[DIST_CALC_RESIDUAL] = 0;
                for (int32_t i = 0; i < CFL_SIGNS; i++) {
                    const int32_t joint_sign = PLANE_SIGN_TO_JOINT_SIGN(plane, pn_sign, i);
                    if (i == 0) {
                        candidate_buffer->candidate_ptr->cfl_alpha_idx =
                            (c << CFL_ALPHABET_SIZE_LOG2) + c;
                        candidate_buffer->candidate_ptr->cfl_alpha_signs = joint_sign;

                        av1_cost_calc_cfl(pcs_ptr,
                                          candidate_buffer,
                                          sb_ptr,
                                          context_ptr,
                                          (plane == 0) ? COMPONENT_CHROMA_CB : COMPONENT_CHROMA_CR,
                                          input_picture_ptr,
                                          input_cb_origin_in_index,
                                          blk_chroma_origin_index,
                                          full_distortion,
                                          &coeff_bits,
                                          0);

                        if (coeff_bits == INT64_MAX)
                            break;
                    }

                    const int32_t alpha_rate =
#if CLN_FAST_COST
                        context_ptr->md_rate_estimation_ptr
                            ->cfl_alpha_fac_bits[joint_sign][plane][c];
#else
                        candidate_buffer->candidate_ptr->md_rate_estimation_ptr
                            ->cfl_alpha_fac_bits[joint_sign][plane][c];
#endif
                    int64_t this_rd = RDCOST(
                        full_lambda, coeff_bits + alpha_rate, full_distortion[DIST_CALC_RESIDUAL]);
                    if (this_rd >= best_rd_uv[joint_sign][plane])
                        continue;
                    best_rd_uv[joint_sign][plane] = this_rd;
                    best_c[joint_sign][plane]     = c;
                    flag                          = 2;
                    if (best_rd_uv[joint_sign][!plane] == INT64_MAX)
                        continue;
                    this_rd += mode_rd + best_rd_uv[joint_sign][!plane];
                    if (this_rd >= best_rd)
                        continue;
                    best_rd         = this_rd;
                    best_joint_sign = joint_sign;
                }
                progress += flag;
            }
        }
    }

    // Compare with DC Chroma
    coeff_bits                          = 0;
    full_distortion[DIST_CALC_RESIDUAL] = 0;

    candidate_buffer->candidate_ptr->cfl_alpha_idx   = 0;
    candidate_buffer->candidate_ptr->cfl_alpha_signs = 0;
#if CLN_FAST_COST
    const int64_t dc_mode_rd = RDCOST(
        full_lambda,
        context_ptr->md_rate_estimation_ptr
            ->intra_uv_mode_fac_bits[CFL_ALLOWED][candidate_buffer->candidate_ptr->intra_luma_mode]
                                    [UV_DC_PRED],
        0);
#else
    const int64_t dc_mode_rd = RDCOST(
        full_lambda,
        candidate_buffer->candidate_ptr->md_rate_estimation_ptr
            ->intra_uv_mode_fac_bits[CFL_ALLOWED][candidate_buffer->candidate_ptr->intra_luma_mode]
                                    [UV_DC_PRED],
        0);
#endif
    av1_cost_calc_cfl(pcs_ptr,
                      candidate_buffer,
                      sb_ptr,
                      context_ptr,
                      COMPONENT_CHROMA,
                      input_picture_ptr,
                      input_cb_origin_in_index,
                      blk_chroma_origin_index,
                      full_distortion,
                      &coeff_bits,
                      1);

    int64_t dc_rd = RDCOST(full_lambda, coeff_bits, full_distortion[DIST_CALC_RESIDUAL]);
    dc_rd += dc_mode_rd;
    if (dc_rd <= best_rd || best_rd == INT64_MAX) {
        candidate_buffer->candidate_ptr->intra_chroma_mode = UV_DC_PRED;
        candidate_buffer->candidate_ptr->cfl_alpha_idx     = 0;
        candidate_buffer->candidate_ptr->cfl_alpha_signs   = 0;
    } else {
        candidate_buffer->candidate_ptr->intra_chroma_mode = UV_CFL_PRED;
        int32_t ind                                        = 0;
        if (best_joint_sign >= 0) {
            const int32_t u = best_c[best_joint_sign][CFL_PRED_U];
            const int32_t v = best_c[best_joint_sign][CFL_PRED_V];
            ind             = (u << CFL_ALPHABET_SIZE_LOG2) + v;
        } else
            best_joint_sign = 0;
        candidate_buffer->candidate_ptr->cfl_alpha_idx   = ind;
        candidate_buffer->candidate_ptr->cfl_alpha_signs = best_joint_sign;
    }
}
// If mode is CFL:
// 1: recon the Luma
// 2: Form the pred_buf_q3
// 3: Loop over alphas and find the best or choose DC
// 4: Recalculate the residual for chroma
static void cfl_prediction(PictureControlSet *          pcs_ptr,
                           ModeDecisionCandidateBuffer *candidate_buffer, SuperBlock *sb_ptr,
                           ModeDecisionContext *context_ptr, EbPictureBufferDesc *input_picture_ptr,
                           uint32_t input_cb_origin_in_index, uint32_t blk_chroma_origin_index) {
    if (context_ptr->blk_geom->has_uv) {
        // 1: recon the Luma
        av1_perform_inverse_transform_recon_luma(context_ptr, candidate_buffer);

        uint32_t rec_luma_offset = ((context_ptr->blk_geom->origin_y >> 3) << 3) *
                candidate_buffer->recon_ptr->stride_y +
            ((context_ptr->blk_geom->origin_x >> 3) << 3);
        // 2: Form the pred_buf_q3
        uint32_t chroma_width  = context_ptr->blk_geom->bwidth_uv;
        uint32_t chroma_height = context_ptr->blk_geom->bheight_uv;

        // Down sample Luma
        if (!context_ptr->hbd_mode_decision) {
            svt_cfl_luma_subsampling_420_lbd(
                &(context_ptr->cfl_temp_luma_recon[rec_luma_offset]),
                candidate_buffer->recon_ptr->stride_y,
                context_ptr->pred_buf_q3,
                context_ptr->blk_geom->bwidth_uv == context_ptr->blk_geom->bwidth
                    ? (context_ptr->blk_geom->bwidth_uv << 1)
                    : context_ptr->blk_geom->bwidth,
                context_ptr->blk_geom->bheight_uv == context_ptr->blk_geom->bheight
                    ? (context_ptr->blk_geom->bheight_uv << 1)
                    : context_ptr->blk_geom->bheight);
        } else {
            svt_cfl_luma_subsampling_420_hbd(
                context_ptr->cfl_temp_luma_recon16bit + rec_luma_offset,
                candidate_buffer->recon_ptr->stride_y,
                context_ptr->pred_buf_q3,
                context_ptr->blk_geom->bwidth_uv == context_ptr->blk_geom->bwidth
                    ? (context_ptr->blk_geom->bwidth_uv << 1)
                    : context_ptr->blk_geom->bwidth,
                context_ptr->blk_geom->bheight_uv == context_ptr->blk_geom->bheight
                    ? (context_ptr->blk_geom->bheight_uv << 1)
                    : context_ptr->blk_geom->bheight);
        }
        int32_t round_offset = chroma_width * chroma_height / 2;

        svt_subtract_average(context_ptr->pred_buf_q3,
                             chroma_width,
                             chroma_height,
                             round_offset,
                             svt_log2f(chroma_width) + svt_log2f(chroma_height));

        // 3: Loop over alphas and find the best or choose DC
        md_cfl_rd_pick_alpha(pcs_ptr,
                             candidate_buffer,
                             sb_ptr,
                             context_ptr,
                             input_picture_ptr,
                             input_cb_origin_in_index,
                             blk_chroma_origin_index);

        if (candidate_buffer->candidate_ptr->intra_chroma_mode == UV_CFL_PRED) {
            // 4: Recalculate the prediction and the residual
            int32_t alpha_q3_cb = cfl_idx_to_alpha(candidate_buffer->candidate_ptr->cfl_alpha_idx,
                                                   candidate_buffer->candidate_ptr->cfl_alpha_signs,
                                                   CFL_PRED_U);
            int32_t alpha_q3_cr = cfl_idx_to_alpha(candidate_buffer->candidate_ptr->cfl_alpha_idx,
                                                   candidate_buffer->candidate_ptr->cfl_alpha_signs,
                                                   CFL_PRED_V);

            assert(chroma_height * CFL_BUF_LINE + chroma_width <= CFL_BUF_SQUARE);

            if (!context_ptr->hbd_mode_decision) {
                svt_cfl_predict_lbd(
                    context_ptr->pred_buf_q3,
                    &(candidate_buffer->prediction_ptr->buffer_cb[blk_chroma_origin_index]),
                    candidate_buffer->prediction_ptr->stride_cb,
                    &(candidate_buffer->prediction_ptr->buffer_cb[blk_chroma_origin_index]),
                    candidate_buffer->prediction_ptr->stride_cb,
                    alpha_q3_cb,
                    8,
                    chroma_width,
                    chroma_height);

                svt_cfl_predict_lbd(
                    context_ptr->pred_buf_q3,
                    &(candidate_buffer->prediction_ptr->buffer_cr[blk_chroma_origin_index]),
                    candidate_buffer->prediction_ptr->stride_cr,
                    &(candidate_buffer->prediction_ptr->buffer_cr[blk_chroma_origin_index]),
                    candidate_buffer->prediction_ptr->stride_cr,
                    alpha_q3_cr,
                    8,
                    chroma_width,
                    chroma_height);
            } else {
                svt_cfl_predict_hbd(context_ptr->pred_buf_q3,
                                    ((uint16_t *)candidate_buffer->prediction_ptr->buffer_cb) +
                                        blk_chroma_origin_index,
                                    candidate_buffer->prediction_ptr->stride_cb,
                                    ((uint16_t *)candidate_buffer->prediction_ptr->buffer_cb) +
                                        blk_chroma_origin_index,
                                    candidate_buffer->prediction_ptr->stride_cb,
                                    alpha_q3_cb,
                                    10,
                                    chroma_width,
                                    chroma_height);

                svt_cfl_predict_hbd(context_ptr->pred_buf_q3,
                                    ((uint16_t *)candidate_buffer->prediction_ptr->buffer_cr) +
                                        blk_chroma_origin_index,
                                    candidate_buffer->prediction_ptr->stride_cr,
                                    ((uint16_t *)candidate_buffer->prediction_ptr->buffer_cr) +
                                        blk_chroma_origin_index,
                                    candidate_buffer->prediction_ptr->stride_cr,
                                    alpha_q3_cr,
                                    10,
                                    chroma_width,
                                    chroma_height);
            }

            // Cb Residual
            residual_kernel(input_picture_ptr->buffer_cb,
                            input_cb_origin_in_index,
                            input_picture_ptr->stride_cb,
                            candidate_buffer->prediction_ptr->buffer_cb,
                            blk_chroma_origin_index,
                            candidate_buffer->prediction_ptr->stride_cb,
                            (int16_t *)candidate_buffer->residual_ptr->buffer_cb,
                            blk_chroma_origin_index,
                            candidate_buffer->residual_ptr->stride_cb,
                            context_ptr->hbd_mode_decision,
                            context_ptr->blk_geom->bwidth_uv,
                            context_ptr->blk_geom->bheight_uv);

            // Cr Residual
            residual_kernel(input_picture_ptr->buffer_cr,
                            input_cb_origin_in_index,
                            input_picture_ptr->stride_cr,
                            candidate_buffer->prediction_ptr->buffer_cr,
                            blk_chroma_origin_index,
                            candidate_buffer->prediction_ptr->stride_cr,
                            (int16_t *)candidate_buffer->residual_ptr->buffer_cr,
                            blk_chroma_origin_index,
                            candidate_buffer->residual_ptr->stride_cr,
                            context_ptr->hbd_mode_decision,
                            context_ptr->blk_geom->bwidth_uv,
                            context_ptr->blk_geom->bheight_uv);
        } else {
            // Alphas = 0, Preds are the same as DC. Switch to DC mode
            candidate_buffer->candidate_ptr->intra_chroma_mode = UV_DC_PRED;
        }
    }
}
#if RFCTR_INTRA_TX_INIT_FUNC
static INLINE TxType av1_get_tx_type(int32_t is_inter, PredictionMode pred_mode,
                                     UvPredictionMode pred_mode_uv, PlaneType plane_type,
                                     TxSize tx_size, int32_t reduced_tx_set) {
    if (txsize_sqr_up_map[tx_size] > TX_32X32 || plane_type == PLANE_TYPE_Y || is_inter) {
        return DCT_DCT;
    }

    MbModeInfo mbmi;
    mbmi.block_mi.mode    = pred_mode;
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

    // block_size  sb_type = BLOCK_8X8;

    MbModeInfo mbmi;
    mbmi.block_mi.mode = pred_mode;
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
    if (!av1_ext_tx_used[tx_set_type][tx_type])
        return DCT_DCT;
    return tx_type;
}
#endif
void check_best_indepedant_cfl(PictureControlSet *pcs_ptr, EbPictureBufferDesc *input_picture_ptr,
                               ModeDecisionContext *context_ptr, uint32_t input_cb_origin_in_index,
                               uint32_t                     blk_chroma_origin_index,
                               ModeDecisionCandidateBuffer *candidate_buffer, uint8_t cb_qindex,
                               uint8_t cr_qindex, uint64_t *cb_full_distortion,
                               uint64_t *cr_full_distortion, uint64_t *cb_coeff_bits,
                               uint64_t *cr_coeff_bits) {
    uint32_t full_lambda = context_ptr->hbd_mode_decision
        ? context_ptr->full_lambda_md[EB_10_BIT_MD]
        : context_ptr->full_lambda_md[EB_8_BIT_MD];
    if (candidate_buffer->candidate_ptr->filter_intra_mode != FILTER_INTRA_MODES)
        assert(candidate_buffer->candidate_ptr->intra_luma_mode == DC_PRED);
    FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;
    // cfl cost
    uint64_t chroma_rate = 0;
    if (candidate_buffer->candidate_ptr->intra_chroma_mode == UV_CFL_PRED) {
        chroma_rate +=
#if CLN_FAST_COST
            context_ptr
                ->md_rate_estimation_ptr
#else
            candidate_buffer->candidate_ptr
                ->md_rate_estimation_ptr
#endif
                ->cfl_alpha_fac_bits[candidate_buffer->candidate_ptr->cfl_alpha_signs][CFL_PRED_U]
                                    [CFL_IDX_U(candidate_buffer->candidate_ptr->cfl_alpha_idx)] +
#if CLN_FAST_COST
            context_ptr
                ->md_rate_estimation_ptr
#else
            candidate_buffer->candidate_ptr
                ->md_rate_estimation_ptr
#endif
                ->cfl_alpha_fac_bits[candidate_buffer->candidate_ptr->cfl_alpha_signs][CFL_PRED_V]
                                    [CFL_IDX_V(candidate_buffer->candidate_ptr->cfl_alpha_idx)];

        chroma_rate +=
#if CLN_FAST_COST
            (uint64_t)context_ptr
                ->md_rate_estimation_ptr
#else
            (uint64_t)candidate_buffer->candidate_ptr
                ->md_rate_estimation_ptr
#endif
                ->intra_uv_mode_fac_bits[CFL_ALLOWED][candidate_buffer->candidate_ptr
                                                          ->intra_luma_mode][UV_CFL_PRED];
        chroma_rate -=
#if CLN_FAST_COST
            (uint64_t)context_ptr
                ->md_rate_estimation_ptr
#else
            (uint64_t)candidate_buffer->candidate_ptr
                ->md_rate_estimation_ptr
#endif
                ->intra_uv_mode_fac_bits[CFL_ALLOWED][candidate_buffer->candidate_ptr
                                                          ->intra_luma_mode][UV_DC_PRED];
    } else
        chroma_rate =
#if CLN_FAST_COST
            (uint64_t)context_ptr
                ->md_rate_estimation_ptr
#else
            (uint64_t)candidate_buffer->candidate_ptr
                ->md_rate_estimation_ptr
#endif
                ->intra_uv_mode_fac_bits[CFL_ALLOWED][candidate_buffer->candidate_ptr
                                                          ->intra_luma_mode][UV_DC_PRED];
    int coeff_rate = (int)(*cb_coeff_bits + *cr_coeff_bits);
    int distortion = (int)(cb_full_distortion[DIST_CALC_RESIDUAL] +
                           cr_full_distortion[DIST_CALC_RESIDUAL]);
    int rate = (int)(coeff_rate + chroma_rate + candidate_buffer->candidate_ptr->fast_luma_rate);
    uint64_t cfl_uv_cost = RDCOST(full_lambda, rate, distortion);

    // cfl vs. best independant
    if (context_ptr->best_uv_cost[candidate_buffer->candidate_ptr->intra_luma_mode]
                                 [3 + candidate_buffer->candidate_ptr->angle_delta[PLANE_TYPE_Y]] <
        cfl_uv_cost) {
        // Update the current candidate
        candidate_buffer->candidate_ptr->intra_chroma_mode =
            context_ptr->best_uv_mode[candidate_buffer->candidate_ptr->intra_luma_mode]
                                     [MAX_ANGLE_DELTA +
                                      candidate_buffer->candidate_ptr->angle_delta[PLANE_TYPE_Y]];
        candidate_buffer->candidate_ptr->angle_delta[PLANE_TYPE_UV] =
            context_ptr->best_uv_angle[candidate_buffer->candidate_ptr->intra_luma_mode]
                                      [MAX_ANGLE_DELTA +
                                       candidate_buffer->candidate_ptr->angle_delta[PLANE_TYPE_Y]];
        candidate_buffer->candidate_ptr
            ->is_directional_chroma_mode_flag = (uint8_t)av1_is_directional_mode((PredictionMode)(
            context_ptr->best_uv_mode[candidate_buffer->candidate_ptr->intra_luma_mode]
                                     [MAX_ANGLE_DELTA +
                                      candidate_buffer->candidate_ptr->angle_delta[PLANE_TYPE_Y]]));

        // check if candidate_buffer->candidate_ptr->fast_luma_rate = context_ptr->fast_luma_rate[candidate_buffer->candidate_ptr->intra_luma_mode];
        candidate_buffer->candidate_ptr->fast_chroma_rate =
            context_ptr
                ->fast_chroma_rate[candidate_buffer->candidate_ptr->intra_luma_mode]
                                  [MAX_ANGLE_DELTA +
                                   candidate_buffer->candidate_ptr->angle_delta[PLANE_TYPE_Y]];

#if RFCTR_INTRA_TX_INIT_FUNC
        candidate_buffer->candidate_ptr->transform_type_uv = av1_get_tx_type(
            0, // is_inter
            (PredictionMode)0,
            (UvPredictionMode)context_ptr
                ->best_uv_mode[candidate_buffer->candidate_ptr->intra_luma_mode]
                              [3 + candidate_buffer->candidate_ptr->angle_delta[PLANE_TYPE_Y]],
            PLANE_TYPE_UV,
            context_ptr->blk_geom->txsize_uv[0][0],
            frm_hdr->reduced_tx_set);
#else
        candidate_buffer->candidate_ptr->transform_type_uv = av1_get_tx_type(
            context_ptr->blk_geom->bsize,
            0,
            (PredictionMode)NULL,
            (UvPredictionMode)context_ptr
                ->best_uv_mode[candidate_buffer->candidate_ptr->intra_luma_mode]
                              [3 + candidate_buffer->candidate_ptr->angle_delta[PLANE_TYPE_Y]],
            PLANE_TYPE_UV,
            0,
            0,
            0,
            context_ptr->blk_geom->txsize_uv[0][0],
            frm_hdr->reduced_tx_set);
#endif
        context_ptr->uv_intra_comp_only = EB_TRUE;

        memset(candidate_buffer->candidate_ptr->eob[1], 0, sizeof(uint16_t));
        memset(candidate_buffer->candidate_ptr->eob[2], 0, sizeof(uint16_t));
        candidate_buffer->candidate_ptr->u_has_coeff = 0;
        candidate_buffer->candidate_ptr->v_has_coeff = 0;
        cb_full_distortion[DIST_CALC_RESIDUAL]       = 0;
        cr_full_distortion[DIST_CALC_RESIDUAL]       = 0;
        cb_full_distortion[DIST_CALC_PREDICTION]     = 0;
        cr_full_distortion[DIST_CALC_PREDICTION]     = 0;

        *cb_coeff_bits = 0;
        *cr_coeff_bits = 0;

        uint32_t count_non_zero_coeffs[3][MAX_NUM_OF_TU_PER_CU];
        context_ptr->md_staging_skip_chroma_pred = EB_FALSE;
        svt_product_prediction_fun_table[candidate_buffer->candidate_ptr->type](
            context_ptr->hbd_mode_decision, context_ptr, pcs_ptr, candidate_buffer);

        // Cb Residual
        residual_kernel(input_picture_ptr->buffer_cb,
                        input_cb_origin_in_index,
                        input_picture_ptr->stride_cb,
                        candidate_buffer->prediction_ptr->buffer_cb,
                        blk_chroma_origin_index,
                        candidate_buffer->prediction_ptr->stride_cb,
                        (int16_t *)candidate_buffer->residual_ptr->buffer_cb,
                        blk_chroma_origin_index,
                        candidate_buffer->residual_ptr->stride_cb,
                        context_ptr->hbd_mode_decision,
                        context_ptr->blk_geom->bwidth_uv,
                        context_ptr->blk_geom->bheight_uv);

        // Cr Residual
        residual_kernel(input_picture_ptr->buffer_cr,
                        input_cb_origin_in_index,
                        input_picture_ptr->stride_cr,
                        candidate_buffer->prediction_ptr->buffer_cr,
                        blk_chroma_origin_index,
                        candidate_buffer->prediction_ptr->stride_cr,
                        (int16_t *)candidate_buffer->residual_ptr->buffer_cr,
                        blk_chroma_origin_index,
                        candidate_buffer->residual_ptr->stride_cr,
                        context_ptr->hbd_mode_decision,
                        context_ptr->blk_geom->bwidth_uv,
                        context_ptr->blk_geom->bheight_uv);

        full_loop_r(context_ptr->sb_ptr,
                    candidate_buffer,
                    context_ptr,
                    input_picture_ptr,
                    pcs_ptr,
                    PICTURE_BUFFER_DESC_CHROMA_MASK,
                    cb_qindex,
                    cr_qindex,
                    &(*count_non_zero_coeffs[1]),
                    &(*count_non_zero_coeffs[2]));

        cu_full_distortion_fast_txb_mode_r(context_ptr->sb_ptr,
                                           candidate_buffer,
                                           context_ptr,
                                           candidate_buffer->candidate_ptr,
                                           pcs_ptr,
                                           input_picture_ptr,
                                           cb_full_distortion,
                                           cr_full_distortion,
                                           count_non_zero_coeffs,
                                           COMPONENT_CHROMA,
                                           cb_coeff_bits,
                                           cr_coeff_bits,
                                           1);
    }
}

EbErrorType av1_first_pass_intra_luma_prediction(
    EbPictureBufferDesc *src, uint32_t src_luma_origin_index, ModeDecisionContext *md_context_ptr,
    PictureControlSet *pcs_ptr, ModeDecisionCandidateBuffer *candidate_buffer_ptr) {
    EbErrorType return_error = EB_ErrorNone;
    uint8_t     is_inter     = 0; // set to 0 b/c this is an intra path

    uint16_t txb_origin_x = md_context_ptr->blk_origin_x +
        md_context_ptr->blk_geom
            ->tx_org_x[is_inter][md_context_ptr->tx_depth][md_context_ptr->txb_itr] -
        md_context_ptr->blk_geom->origin_x;
    uint16_t txb_origin_y = md_context_ptr->blk_origin_y +
        md_context_ptr->blk_geom
            ->tx_org_y[is_inter][md_context_ptr->tx_depth][md_context_ptr->txb_itr] -
        md_context_ptr->blk_geom->origin_y;
    uint8_t tx_width =
        md_context_ptr->blk_geom->tx_width[md_context_ptr->tx_depth][md_context_ptr->txb_itr];
    uint8_t tx_height =
        md_context_ptr->blk_geom->tx_height[md_context_ptr->tx_depth][md_context_ptr->txb_itr];

    uint32_t mode_type_left_neighbor_index = get_neighbor_array_unit_left_index(
        md_context_ptr->mode_type_neighbor_array, txb_origin_y);
    uint32_t mode_type_top_neighbor_index = get_neighbor_array_unit_top_index(
        md_context_ptr->mode_type_neighbor_array, txb_origin_x);
    uint32_t intra_luma_mode_left_neighbor_index = get_neighbor_array_unit_left_index(
        md_context_ptr->intra_luma_mode_neighbor_array, txb_origin_y);
    uint32_t intra_luma_mode_top_neighbor_index = get_neighbor_array_unit_top_index(
        md_context_ptr->intra_luma_mode_neighbor_array, txb_origin_x);

    md_context_ptr->intra_luma_left_mode = (uint32_t)(
        (md_context_ptr->mode_type_neighbor_array->left_array[mode_type_left_neighbor_index] !=
         INTRA_MODE)
            ? DC_PRED /*EB_INTRA_DC*/
            : (uint32_t)md_context_ptr->intra_luma_mode_neighbor_array
                  ->left_array[intra_luma_mode_left_neighbor_index]);

    md_context_ptr->intra_luma_top_mode = (uint32_t)(
        (md_context_ptr->mode_type_neighbor_array->top_array[mode_type_top_neighbor_index] !=
         INTRA_MODE)
            ? DC_PRED /*EB_INTRA_DC*/
            : (uint32_t)md_context_ptr->intra_luma_mode_neighbor_array->top_array
                  [intra_luma_mode_top_neighbor_index]); //   use DC. This seems like we could use a SB-width

    TxSize tx_size =
        md_context_ptr->blk_geom->txsize[md_context_ptr->tx_depth][md_context_ptr->txb_itr];
    PredictionMode mode;
    if (!md_context_ptr->hbd_mode_decision) {
        uint8_t top_neigh_array[64 * 2 + 1];
        uint8_t left_neigh_array[64 * 2 + 1];

        if (txb_origin_y != 0)
            svt_memcpy(
                top_neigh_array + 1,
                src->buffer_y + src_luma_origin_index - src->stride_y,
                //md_context_ptr->tx_search_luma_recon_neighbor_array->top_array + txb_origin_x,
                tx_width * 2);
        if (txb_origin_x != 0)
            pic_copy_kernel_8bit(&(src->buffer_y[src_luma_origin_index - 1]),
                                 src->stride_y,
                                 &(left_neigh_array[1]),
                                 1,
                                 1,
                                 tx_height * 2);

        //svt_memcpy(left_neigh_array + 1,
        //       md_context_ptr->tx_search_luma_recon_neighbor_array->left_array + txb_origin_y,
        //       tx_height * 2);
        if (txb_origin_y != 0 && txb_origin_x != 0)
            top_neigh_array[0] = left_neigh_array[0] =
                src->buffer_y[src_luma_origin_index - 1 - src->stride_y];
        //md_context_ptr->tx_search_luma_recon_neighbor_array
        //    ->top_left_array[MAX_PICTURE_HEIGHT_SIZE + txb_origin_x - txb_origin_y];

        mode = candidate_buffer_ptr->candidate_ptr->pred_mode;
        svt_av1_predict_intra_block(
#if !OPT_INIT_XD_2
            &md_context_ptr->sb_ptr->tile_info,
#endif
            !ED_STAGE,
            md_context_ptr->blk_geom,
#if OPT_INIT_XD_2
            md_context_ptr->blk_ptr->av1xd,
#else
            pcs_ptr->parent_pcs_ptr->av1_cm, //const Av1Common *cm,
#endif
            md_context_ptr->blk_geom->bwidth,
            md_context_ptr->blk_geom->bheight,
            tx_size,
            mode, //PredictionMode mode,
            candidate_buffer_ptr->candidate_ptr->angle_delta[PLANE_TYPE_Y],
            candidate_buffer_ptr->candidate_ptr->palette_info
                ? (candidate_buffer_ptr->candidate_ptr->palette_info->pmi.palette_size[0] > 0)
                : 0,
            candidate_buffer_ptr->candidate_ptr->palette_info, //ATB MD
            candidate_buffer_ptr->candidate_ptr->filter_intra_mode,
            top_neigh_array + 1,
            left_neigh_array + 1,
            candidate_buffer_ptr->prediction_ptr, //uint8_t *dst,
            (md_context_ptr->blk_geom
                 ->tx_org_x[is_inter][md_context_ptr->tx_depth][md_context_ptr->txb_itr] -
             md_context_ptr->blk_geom->origin_x) >>
                2,
            (md_context_ptr->blk_geom
                 ->tx_org_y[is_inter][md_context_ptr->tx_depth][md_context_ptr->txb_itr] -
             md_context_ptr->blk_geom->origin_y) >>
                2,
            PLANE_TYPE_Y, //int32_t plane,
            md_context_ptr->blk_geom->bsize,
            md_context_ptr->blk_origin_x,
            md_context_ptr->blk_origin_y,
            md_context_ptr->blk_origin_x,
            md_context_ptr->blk_origin_y,
            md_context_ptr->blk_geom
                ->tx_org_x[is_inter][md_context_ptr->tx_depth]
                          [md_context_ptr->txb_itr], //uint32_t cuOrgX used only for prediction Ptr
            md_context_ptr->blk_geom
                ->tx_org_y[is_inter][md_context_ptr->tx_depth]
                          [md_context_ptr->txb_itr], //uint32_t cuOrgY used only for prediction Ptr
#if !OPT_INIT_XD_2
            pcs_ptr->mi_grid_base,
#endif
            &((SequenceControlSet *)pcs_ptr->scs_wrapper_ptr->object_ptr)->seq_header);
    } else {
        uint16_t top_neigh_array[64 * 2 + 1];
        uint16_t left_neigh_array[64 * 2 + 1];

        if (txb_origin_y != 0)
            svt_memcpy(
                top_neigh_array + 1,
                (uint16_t *)(md_context_ptr->tx_search_luma_recon_neighbor_array16bit->top_array) +
                    txb_origin_x,
                sizeof(uint16_t) * tx_width * 2);
        if (txb_origin_x != 0)
            svt_memcpy(
                left_neigh_array + 1,
                (uint16_t *)(md_context_ptr->tx_search_luma_recon_neighbor_array16bit->left_array) +
                    txb_origin_y,
                sizeof(uint16_t) * tx_height * 2);
        if (txb_origin_y != 0 && txb_origin_x != 0)
            top_neigh_array[0] = left_neigh_array[0] =
                ((uint16_t *)(md_context_ptr->tx_search_luma_recon_neighbor_array16bit
                                  ->top_left_array) +
                 MAX_PICTURE_HEIGHT_SIZE + txb_origin_x - txb_origin_y)[0];

        mode = candidate_buffer_ptr->candidate_ptr->pred_mode;
        svt_av1_predict_intra_block_16bit(
            EB_10BIT,
#if !OPT_INIT_XD_2
            &md_context_ptr->sb_ptr->tile_info,
#endif
            !ED_STAGE,
            md_context_ptr->blk_geom,
#if OPT_INIT_XD_2
            md_context_ptr->blk_ptr->av1xd,
#else
            pcs_ptr->parent_pcs_ptr->av1_cm,
#endif
            md_context_ptr->blk_geom->bwidth,
            md_context_ptr->blk_geom->bheight,
            tx_size,
            mode,
            candidate_buffer_ptr->candidate_ptr->angle_delta[PLANE_TYPE_Y],
            candidate_buffer_ptr->candidate_ptr->palette_info
                ? (candidate_buffer_ptr->candidate_ptr->palette_info->pmi.palette_size[0] > 0)
                : 0,
            candidate_buffer_ptr->candidate_ptr->palette_info, //ATB MD
            candidate_buffer_ptr->candidate_ptr->filter_intra_mode,
            top_neigh_array + 1,
            left_neigh_array + 1,
            candidate_buffer_ptr->prediction_ptr,
            (md_context_ptr->blk_geom
                 ->tx_org_x[is_inter][md_context_ptr->tx_depth][md_context_ptr->txb_itr] -
             md_context_ptr->blk_geom->origin_x) >>
                2,
            (md_context_ptr->blk_geom
                 ->tx_org_y[is_inter][md_context_ptr->tx_depth][md_context_ptr->txb_itr] -
             md_context_ptr->blk_geom->origin_y) >>
                2,
            PLANE_TYPE_Y,
            md_context_ptr->blk_geom->bsize,
            md_context_ptr->blk_origin_x,
            md_context_ptr->blk_origin_y,
            md_context_ptr->blk_origin_x,
            md_context_ptr->blk_origin_y,
            md_context_ptr->blk_geom
                ->tx_org_x[is_inter][md_context_ptr->tx_depth]
                          [md_context_ptr->txb_itr], //uint32_t cuOrgX used only for prediction Ptr
            md_context_ptr->blk_geom
                ->tx_org_y[is_inter][md_context_ptr->tx_depth]
                          [md_context_ptr->txb_itr], //uint32_t cuOrgY used only for prediction Ptr
#if !OPT_INIT_XD_2
            pcs_ptr->mi_grid_base,
#endif
            &((SequenceControlSet *)pcs_ptr->scs_wrapper_ptr->object_ptr)->seq_header);
    }

    return return_error;
}
// double check the usage of tx_search_luma_recon_neighbor_array16bit
EbErrorType av1_intra_luma_prediction(ModeDecisionContext *        md_context_ptr,
                                      PictureControlSet *          pcs_ptr,
                                      ModeDecisionCandidateBuffer *candidate_buffer_ptr) {
    EbErrorType return_error = EB_ErrorNone;
    uint8_t     is_inter     = 0; // set to 0 b/c this is an intra path

    uint16_t txb_origin_x = md_context_ptr->blk_origin_x +
        md_context_ptr->blk_geom
            ->tx_org_x[is_inter][md_context_ptr->tx_depth][md_context_ptr->txb_itr] -
        md_context_ptr->blk_geom->origin_x;
    uint16_t txb_origin_y = md_context_ptr->blk_origin_y +
        md_context_ptr->blk_geom
            ->tx_org_y[is_inter][md_context_ptr->tx_depth][md_context_ptr->txb_itr] -
        md_context_ptr->blk_geom->origin_y;
    uint8_t tx_width =
        md_context_ptr->blk_geom->tx_width[md_context_ptr->tx_depth][md_context_ptr->txb_itr];
    uint8_t tx_height =
        md_context_ptr->blk_geom->tx_height[md_context_ptr->tx_depth][md_context_ptr->txb_itr];

    uint32_t mode_type_left_neighbor_index = get_neighbor_array_unit_left_index(
        md_context_ptr->mode_type_neighbor_array, txb_origin_y);
    uint32_t mode_type_top_neighbor_index = get_neighbor_array_unit_top_index(
        md_context_ptr->mode_type_neighbor_array, txb_origin_x);
    uint32_t intra_luma_mode_left_neighbor_index = get_neighbor_array_unit_left_index(
        md_context_ptr->intra_luma_mode_neighbor_array, txb_origin_y);
    uint32_t intra_luma_mode_top_neighbor_index = get_neighbor_array_unit_top_index(
        md_context_ptr->intra_luma_mode_neighbor_array, txb_origin_x);

    md_context_ptr->intra_luma_left_mode = (uint32_t)(
        (md_context_ptr->mode_type_neighbor_array->left_array[mode_type_left_neighbor_index] !=
         INTRA_MODE)
            ? DC_PRED /*EB_INTRA_DC*/
            : (uint32_t)md_context_ptr->intra_luma_mode_neighbor_array
                  ->left_array[intra_luma_mode_left_neighbor_index]);

    md_context_ptr->intra_luma_top_mode = (uint32_t)(
        (md_context_ptr->mode_type_neighbor_array->top_array[mode_type_top_neighbor_index] !=
         INTRA_MODE)
            ? DC_PRED /*EB_INTRA_DC*/
            : (uint32_t)md_context_ptr->intra_luma_mode_neighbor_array->top_array
                  [intra_luma_mode_top_neighbor_index]); //   use DC. This seems like we could use a SB-width

    TxSize tx_size =
        md_context_ptr->blk_geom->txsize[md_context_ptr->tx_depth][md_context_ptr->txb_itr];

    PredictionMode mode;
    if (!md_context_ptr->hbd_mode_decision) {
        uint8_t top_neigh_array[64 * 2 + 1];
        uint8_t left_neigh_array[64 * 2 + 1];

        if (txb_origin_y != 0)
            svt_memcpy(
                top_neigh_array + 1,
                md_context_ptr->tx_search_luma_recon_neighbor_array->top_array + txb_origin_x,
                tx_width * 2);
        if (txb_origin_x != 0)
            svt_memcpy(
                left_neigh_array + 1,
                md_context_ptr->tx_search_luma_recon_neighbor_array->left_array + txb_origin_y,
                tx_height * 2);
        if (txb_origin_y != 0 && txb_origin_x != 0)
            top_neigh_array[0] = left_neigh_array[0] =
                md_context_ptr->tx_search_luma_recon_neighbor_array
                    ->top_left_array[MAX_PICTURE_HEIGHT_SIZE + txb_origin_x - txb_origin_y];

        mode = candidate_buffer_ptr->candidate_ptr->pred_mode;
        svt_av1_predict_intra_block(
#if !OPT_INIT_XD_2
            &md_context_ptr->sb_ptr->tile_info,
#endif
            !ED_STAGE,
            md_context_ptr->blk_geom,
#if OPT_INIT_XD_2
            md_context_ptr->blk_ptr->av1xd,
#else
            pcs_ptr->parent_pcs_ptr->av1_cm, //const Av1Common *cm,
#endif
            md_context_ptr->blk_geom->bwidth,
            md_context_ptr->blk_geom->bheight,
            tx_size,
            mode, //PredictionMode mode,
            candidate_buffer_ptr->candidate_ptr->angle_delta[PLANE_TYPE_Y],
            candidate_buffer_ptr->candidate_ptr->palette_info
                ? (candidate_buffer_ptr->candidate_ptr->palette_info->pmi.palette_size[0] > 0)
                : 0,
            candidate_buffer_ptr->candidate_ptr->palette_info, //ATB MD
            candidate_buffer_ptr->candidate_ptr->filter_intra_mode,
            top_neigh_array + 1,
            left_neigh_array + 1,
            candidate_buffer_ptr->prediction_ptr, //uint8_t *dst,
            (md_context_ptr->blk_geom
                 ->tx_org_x[is_inter][md_context_ptr->tx_depth][md_context_ptr->txb_itr] -
             md_context_ptr->blk_geom->origin_x) >>
                2,
            (md_context_ptr->blk_geom
                 ->tx_org_y[is_inter][md_context_ptr->tx_depth][md_context_ptr->txb_itr] -
             md_context_ptr->blk_geom->origin_y) >>
                2,
            PLANE_TYPE_Y, //int32_t plane,
            md_context_ptr->blk_geom->bsize,
            md_context_ptr->blk_origin_x,
            md_context_ptr->blk_origin_y,
            md_context_ptr->blk_origin_x,
            md_context_ptr->blk_origin_y,
            md_context_ptr->blk_geom
                ->tx_org_x[is_inter][md_context_ptr->tx_depth]
                          [md_context_ptr->txb_itr], //uint32_t cuOrgX used only for prediction Ptr
            md_context_ptr->blk_geom
                ->tx_org_y[is_inter][md_context_ptr->tx_depth]
                          [md_context_ptr->txb_itr], //uint32_t cuOrgY used only for prediction Ptr
#if !OPT_INIT_XD_2
            pcs_ptr->mi_grid_base,
#endif
            &((SequenceControlSet *)pcs_ptr->scs_wrapper_ptr->object_ptr)->seq_header);
    } else {
        uint16_t top_neigh_array[64 * 2 + 1];
        uint16_t left_neigh_array[64 * 2 + 1];

        if (txb_origin_y != 0)
            svt_memcpy(
                top_neigh_array + 1,
                (uint16_t *)(md_context_ptr->tx_search_luma_recon_neighbor_array16bit->top_array) +
                    txb_origin_x,
                sizeof(uint16_t) * tx_width * 2);
        if (txb_origin_x != 0)
            svt_memcpy(
                left_neigh_array + 1,
                (uint16_t *)(md_context_ptr->tx_search_luma_recon_neighbor_array16bit->left_array) +
                    txb_origin_y,
                sizeof(uint16_t) * tx_height * 2);
        if (txb_origin_y != 0 && txb_origin_x != 0)
            top_neigh_array[0] = left_neigh_array[0] =
                ((uint16_t *)(md_context_ptr->tx_search_luma_recon_neighbor_array16bit
                                  ->top_left_array) +
                 MAX_PICTURE_HEIGHT_SIZE + txb_origin_x - txb_origin_y)[0];

        mode = candidate_buffer_ptr->candidate_ptr->pred_mode;
        svt_av1_predict_intra_block_16bit(
            EB_10BIT,
#if !OPT_INIT_XD_2
            &md_context_ptr->sb_ptr->tile_info,
#endif
            !ED_STAGE,
            md_context_ptr->blk_geom,
#if OPT_INIT_XD_2
            md_context_ptr->blk_ptr->av1xd,
#else
            pcs_ptr->parent_pcs_ptr->av1_cm,
#endif
            md_context_ptr->blk_geom->bwidth,
            md_context_ptr->blk_geom->bheight,
            tx_size,
            mode,
            candidate_buffer_ptr->candidate_ptr->angle_delta[PLANE_TYPE_Y],
            candidate_buffer_ptr->candidate_ptr->palette_info
                ? (candidate_buffer_ptr->candidate_ptr->palette_info->pmi.palette_size[0] > 0)
                : 0,
            candidate_buffer_ptr->candidate_ptr->palette_info, //ATB MD
            candidate_buffer_ptr->candidate_ptr->filter_intra_mode,
            top_neigh_array + 1,
            left_neigh_array + 1,
            candidate_buffer_ptr->prediction_ptr,
            (md_context_ptr->blk_geom
                 ->tx_org_x[is_inter][md_context_ptr->tx_depth][md_context_ptr->txb_itr] -
             md_context_ptr->blk_geom->origin_x) >>
                2,
            (md_context_ptr->blk_geom
                 ->tx_org_y[is_inter][md_context_ptr->tx_depth][md_context_ptr->txb_itr] -
             md_context_ptr->blk_geom->origin_y) >>
                2,
            PLANE_TYPE_Y,
            md_context_ptr->blk_geom->bsize,
            md_context_ptr->blk_origin_x,
            md_context_ptr->blk_origin_y,
            md_context_ptr->blk_origin_x,
            md_context_ptr->blk_origin_y,
            md_context_ptr->blk_geom
                ->tx_org_x[is_inter][md_context_ptr->tx_depth]
                          [md_context_ptr->txb_itr], //uint32_t cuOrgX used only for prediction Ptr
            md_context_ptr->blk_geom
                ->tx_org_y[is_inter][md_context_ptr->tx_depth]
                          [md_context_ptr->txb_itr], //uint32_t cuOrgY used only for prediction Ptr
#if !OPT_INIT_XD_2
            pcs_ptr->mi_grid_base,
#endif
            &((SequenceControlSet *)pcs_ptr->scs_wrapper_ptr->object_ptr)->seq_header);
    }

    return return_error;
}

static void tx_search_update_recon_sample_neighbor_array(
    NeighborArrayUnit *lumaReconSampleNeighborArray, EbPictureBufferDesc *recon_buffer,
    uint32_t txb_origin_x, uint32_t txb_origin_y, uint32_t input_origin_x, uint32_t input_origin_y,
    uint32_t width, uint32_t height, EbBool hbd) {
    if (hbd) {
        neighbor_array_unit16bit_sample_write(lumaReconSampleNeighborArray,
                                              (uint16_t *)recon_buffer->buffer_y,
                                              recon_buffer->stride_y,
                                              recon_buffer->origin_x + txb_origin_x,
                                              recon_buffer->origin_y + txb_origin_y,
                                              input_origin_x,
                                              input_origin_y,
                                              width,
                                              height,
                                              NEIGHBOR_ARRAY_UNIT_FULL_MASK);
    } else {
        neighbor_array_unit_sample_write(lumaReconSampleNeighborArray,
                                         recon_buffer->buffer_y,
                                         recon_buffer->stride_y,
                                         recon_buffer->origin_x + txb_origin_x,
                                         recon_buffer->origin_y + txb_origin_y,
                                         input_origin_x,
                                         input_origin_y,
                                         width,
                                         height,
                                         NEIGHBOR_ARRAY_UNIT_FULL_MASK);
    }

    return;
}
uint8_t get_end_tx_depth(BlockSize bsize) {
    uint8_t tx_depth = 0;
    if (bsize == BLOCK_64X64 || bsize == BLOCK_32X32 || bsize == BLOCK_16X16 ||
        bsize == BLOCK_64X32 || bsize == BLOCK_32X64 || bsize == BLOCK_16X32 ||
        bsize == BLOCK_32X16 || bsize == BLOCK_16X8 || bsize == BLOCK_8X16 ||
        bsize == BLOCK_64X16 || bsize == BLOCK_16X64 || bsize == BLOCK_32X8 ||
        bsize == BLOCK_8X32 || bsize == BLOCK_16X4 || bsize == BLOCK_4X16)
        tx_depth = 2;
    else if (bsize == BLOCK_8X8)
        tx_depth = 1;
    // tx_depth=0 if BLOCK_8X4, BLOCK_4X8, BLOCK_4X4, BLOCK_128X128, BLOCK_128X64, BLOCK_64X128
    return tx_depth;
}

uint8_t allowed_txt[6][TX_SIZES_ALL][TX_TYPES] = {
    {{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}},
    //txt_th2
    {{1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},
     {1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // th4
    {{1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0},
     {1, 1, 1, 1, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    //th_35d
    {{1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 1},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0},
     {1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 1},
     {1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // th5d
    {{1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},
     {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // dct_dct and IDXT for SC
    {{1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0}}};

void tx_initialize_neighbor_arrays(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                                   EbBool is_inter) {
    uint16_t tile_idx = context_ptr->tile_index;
    // Set recon neighbor array to be used @ intra compensation
    if (!is_inter) {
        if (context_ptr->hbd_mode_decision)
            context_ptr->tx_search_luma_recon_neighbor_array16bit = context_ptr->tx_depth == 2
                ? pcs_ptr->md_tx_depth_2_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX]
                                                                       [tile_idx]
                : context_ptr->tx_depth == 1
                ? pcs_ptr->md_tx_depth_1_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX]
                                                                       [tile_idx]
                : pcs_ptr->md_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
        else
            context_ptr->tx_search_luma_recon_neighbor_array = context_ptr->tx_depth == 2
                ? pcs_ptr
                      ->md_tx_depth_2_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx]
                : context_ptr->tx_depth == 1
                ? pcs_ptr
                      ->md_tx_depth_1_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx]
                : pcs_ptr->md_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    }
    // Set luma dc sign level coeff
    context_ptr->full_loop_luma_dc_sign_level_coeff_neighbor_array = (context_ptr->tx_depth)
        ? pcs_ptr->md_tx_depth_1_luma_dc_sign_level_coeff_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX]
                                                                        [tile_idx]
        : pcs_ptr->md_luma_dc_sign_level_coeff_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
}

void tx_update_neighbor_arrays(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                               ModeDecisionCandidateBuffer *candidate_buffer, EbBool is_inter) {
    uint16_t tile_idx = context_ptr->tile_index;
    if (context_ptr->tx_depth) {
        if (!is_inter)
            tx_search_update_recon_sample_neighbor_array(
                context_ptr->hbd_mode_decision
                    ? context_ptr->tx_search_luma_recon_neighbor_array16bit
                    : context_ptr->tx_search_luma_recon_neighbor_array,
                candidate_buffer->recon_ptr,
                context_ptr->blk_geom
                    ->tx_org_x[is_inter][context_ptr->tx_depth][context_ptr->txb_itr],
                context_ptr->blk_geom
                    ->tx_org_y[is_inter][context_ptr->tx_depth][context_ptr->txb_itr],
                context_ptr->sb_origin_x +
                    context_ptr->blk_geom
                        ->tx_org_x[is_inter][context_ptr->tx_depth][context_ptr->txb_itr],
                context_ptr->sb_origin_y +
                    context_ptr->blk_geom
                        ->tx_org_y[is_inter][context_ptr->tx_depth][context_ptr->txb_itr],
                context_ptr->blk_geom->tx_width[context_ptr->tx_depth][context_ptr->txb_itr],
                context_ptr->blk_geom->tx_height[context_ptr->tx_depth][context_ptr->txb_itr],
                context_ptr->hbd_mode_decision);

        int8_t dc_sign_level_coeff =
            candidate_buffer->candidate_ptr->quantized_dc[0][context_ptr->txb_itr];
        neighbor_array_unit_mode_write(
            pcs_ptr->md_tx_depth_1_luma_dc_sign_level_coeff_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX]
                                                                          [tile_idx],
            (uint8_t *)&dc_sign_level_coeff,
            context_ptr->sb_origin_x +
                context_ptr->blk_geom
                    ->tx_org_x[is_inter][context_ptr->tx_depth][context_ptr->txb_itr],
            context_ptr->sb_origin_y +
                context_ptr->blk_geom
                    ->tx_org_y[is_inter][context_ptr->tx_depth][context_ptr->txb_itr],
            context_ptr->blk_geom->tx_width[context_ptr->tx_depth][context_ptr->txb_itr],
            context_ptr->blk_geom->tx_height[context_ptr->tx_depth][context_ptr->txb_itr],
            NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
    }
}
void tx_reset_neighbor_arrays(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                              EbBool is_inter, uint8_t tx_depth) {
    int      sb_size  = pcs_ptr->parent_pcs_ptr->scs_ptr->static_config.super_block_size;
    uint16_t tile_idx = context_ptr->tile_index;
    if (tx_depth) {
        if (!is_inter) {
            if (context_ptr->hbd_mode_decision) {
                if (tx_depth == 2) {
                    copy_neigh_arr(
                        pcs_ptr
                            ->md_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX][tile_idx],
                        pcs_ptr
                            ->md_tx_depth_2_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX]
                                                                          [tile_idx],
                        context_ptr->sb_origin_x + context_ptr->blk_geom->origin_x,
                        context_ptr->sb_origin_y + context_ptr->blk_geom->origin_y,
                        context_ptr->blk_geom->bwidth,
                        context_ptr->blk_geom->bheight,
                        NEIGHBOR_ARRAY_UNIT_TOPLEFT_MASK);

                    copy_neigh_arr(
                        pcs_ptr
                            ->md_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX][tile_idx],
                        pcs_ptr
                            ->md_tx_depth_2_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX]
                                                                          [tile_idx],
                        context_ptr->sb_origin_x + context_ptr->blk_geom->origin_x,
                        context_ptr->sb_origin_y + context_ptr->blk_geom->origin_y,
                        context_ptr->blk_geom->bwidth * 2,
                        MIN(context_ptr->blk_geom->bheight * 2,
                            sb_size - context_ptr->blk_geom->origin_y),
                        NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
                } else {
                    copy_neigh_arr(
                        pcs_ptr
                            ->md_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX][tile_idx],
                        pcs_ptr
                            ->md_tx_depth_1_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX]
                                                                          [tile_idx],
                        context_ptr->sb_origin_x + context_ptr->blk_geom->origin_x,
                        context_ptr->sb_origin_y + context_ptr->blk_geom->origin_y,
                        context_ptr->blk_geom->bwidth,
                        context_ptr->blk_geom->bheight,
                        NEIGHBOR_ARRAY_UNIT_TOPLEFT_MASK);

                    copy_neigh_arr(
                        pcs_ptr
                            ->md_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX][tile_idx],
                        pcs_ptr
                            ->md_tx_depth_1_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX]
                                                                          [tile_idx],
                        context_ptr->sb_origin_x + context_ptr->blk_geom->origin_x,
                        context_ptr->sb_origin_y + context_ptr->blk_geom->origin_y,
                        context_ptr->blk_geom->bwidth * 2,
                        MIN(context_ptr->blk_geom->bheight * 2,
                            sb_size - context_ptr->blk_geom->origin_y),
                        NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
                }
            } else {
                if (tx_depth == 2) {
                    copy_neigh_arr(
                        pcs_ptr->md_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx],
                        pcs_ptr->md_tx_depth_2_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX]
                                                                        [tile_idx],
                        context_ptr->sb_origin_x + context_ptr->blk_geom->origin_x,
                        context_ptr->sb_origin_y + context_ptr->blk_geom->origin_y,
                        context_ptr->blk_geom->bwidth,
                        context_ptr->blk_geom->bheight,
                        NEIGHBOR_ARRAY_UNIT_TOPLEFT_MASK);
                    copy_neigh_arr(
                        pcs_ptr->md_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx],
                        pcs_ptr->md_tx_depth_2_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX]
                                                                        [tile_idx],
                        context_ptr->sb_origin_x + context_ptr->blk_geom->origin_x,
                        context_ptr->sb_origin_y + context_ptr->blk_geom->origin_y,
                        context_ptr->blk_geom->bwidth * 2,
                        MIN(context_ptr->blk_geom->bheight * 2,
                            sb_size - context_ptr->blk_geom->origin_y),
                        NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
                } else {
                    copy_neigh_arr(
                        pcs_ptr->md_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx],
                        pcs_ptr->md_tx_depth_1_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX]
                                                                        [tile_idx],
                        context_ptr->sb_origin_x + context_ptr->blk_geom->origin_x,
                        context_ptr->sb_origin_y + context_ptr->blk_geom->origin_y,
                        context_ptr->blk_geom->bwidth,
                        context_ptr->blk_geom->bheight,
                        NEIGHBOR_ARRAY_UNIT_TOPLEFT_MASK);
                    copy_neigh_arr(
                        pcs_ptr->md_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx],
                        pcs_ptr->md_tx_depth_1_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX]
                                                                        [tile_idx],
                        context_ptr->sb_origin_x + context_ptr->blk_geom->origin_x,
                        context_ptr->sb_origin_y + context_ptr->blk_geom->origin_y,
                        context_ptr->blk_geom->bwidth * 2,
                        MIN(context_ptr->blk_geom->bheight * 2,
                            sb_size - context_ptr->blk_geom->origin_y),
                        NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
                }
            }
        }
        copy_neigh_arr(
            pcs_ptr->md_luma_dc_sign_level_coeff_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx],
            pcs_ptr->md_tx_depth_1_luma_dc_sign_level_coeff_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX]
                                                                          [tile_idx],
            context_ptr->sb_origin_x + context_ptr->blk_geom->origin_x,
            context_ptr->sb_origin_y + context_ptr->blk_geom->origin_y,
            context_ptr->blk_geom->bwidth,
            context_ptr->blk_geom->bheight,
            NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
    }
}
void copy_txt_data(ModeDecisionCandidateBuffer *candidate_buffer, ModeDecisionContext *context_ptr,
                   uint32_t txb_origin_index, TxType best_tx_type) {
    uint8_t  tx_depth      = context_ptr->tx_depth;
    uint8_t  txb_itr       = context_ptr->txb_itr;
    uint32_t txb_1d_offset = context_ptr->txb_1d_offset;
    uint8_t  tx_width      = context_ptr->blk_geom->tx_width[tx_depth][txb_itr];
    uint8_t  tx_height     = context_ptr->blk_geom->tx_height[tx_depth][txb_itr];
    // copy recon_coeff_ptr
    memcpy(((int32_t *)candidate_buffer->recon_coeff_ptr->buffer_y) + txb_1d_offset,
           ((int32_t *)context_ptr->recon_coeff_ptr[best_tx_type]->buffer_y) + txb_1d_offset,
           (tx_width * tx_height * sizeof(uint32_t)));

    // copy recon_ptr
    EbPictureBufferDesc *recon_ptr = candidate_buffer->recon_ptr;
    if (context_ptr->hbd_mode_decision) {
        for (uint32_t j = 0; j < tx_height; ++j)
            memcpy(((uint16_t *)recon_ptr->buffer_y) + txb_origin_index + j * recon_ptr->stride_y,
                   ((uint16_t *)context_ptr->recon_ptr[best_tx_type]->buffer_y) + txb_origin_index +
                       j * recon_ptr->stride_y,
                   tx_width * sizeof(uint16_t));
    } else {
        for (uint32_t j = 0; j < tx_height; ++j)
            memcpy(recon_ptr->buffer_y + txb_origin_index + j * recon_ptr->stride_y,
                   context_ptr->recon_ptr[best_tx_type]->buffer_y + txb_origin_index +
                       j * recon_ptr->stride_y,
                   context_ptr->blk_geom->tx_width[tx_depth][txb_itr]);
    }
}
#if !TUNE_REMOVE_TXT_STATS
/*
 * Determine whether to bypass a given tx_type based on statistics of previously chosen tx_types.
 *
 * Inputs:
 * tx_type - corresponds to the current tx_type; this function determines if that tx_type should be evaluated
 *           based on statistics of previous blocks.
 * tx_size - the tx_size of the current block.
 * is_inter - whether the current block uses inter or intra prediction.
 * dct_dct_count_non_zero_coeffs - the number of non-zero coefficients of the DCT_DCT transform of the block (which is always evaluated first)
 *
 * Returns:
 * TRUE if the current tx_type should be evaluated or FALSE if the current tx_type should be skipped.
 */
EbBool bypass_txt_based_on_stats(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                                 int32_t tx_type, TxSize tx_size, EbBool is_inter,
                                 uint16_t dct_dct_count_non_zero_coeffs) {
    int8_t pred_depth_refinement =
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds].pred_depth_refinement;
    // Set the bounds of pred_depth_refinement for array indexing
    pred_depth_refinement = MIN(pred_depth_refinement, 1);
    pred_depth_refinement = MAX(pred_depth_refinement, -1);
    pred_depth_refinement++;
    uint8_t  depth_idx   = (context_ptr->blk_geom->sq_size == 16) ? 0 : 1;
    uint8_t  tx_size_idx = ((tx_size == TX_4X4) || (tx_size == TX_4X8) || (tx_size == TX_8X4)) ? 0
         : ((tx_size == TX_8X8) || (tx_size == TX_8X16) || (tx_size == TX_16X8))               ? 1
                                                                                               : 2;
    uint16_t total_samples =
        (context_ptr->blk_geom->tx_width[context_ptr->tx_depth][context_ptr->txb_itr] *
         context_ptr->blk_geom->tx_height[context_ptr->tx_depth][context_ptr->txb_itr]);
    uint8_t freq_band;
    if (dct_dct_count_non_zero_coeffs >= ((total_samples * 2) / 20)) {
        freq_band = 1;
    } else {
        freq_band = 0;
    }
    if (pcs_ptr->slice_type == I_SLICE) {
        if (is_inter) { // INTER path
            if (inter_txt_cycles_reduction_th[depth_idx][pred_depth_refinement][tx_size_idx]
                                             [freq_band][(tx_type - 1)] <
                context_ptr->txt_ctrls.inter_th)
                return EB_TRUE;
        } else { // INTRA path
            if (intra_txt_cycles_reduction_th[depth_idx][pred_depth_refinement][tx_size_idx]
                                             [freq_band][(tx_type - 1)] <
                context_ptr->txt_ctrls.intra_th)
                return EB_TRUE;
        }
    } else {
        if (is_inter) { // INTER path
            if (context_ptr->txt_prob[pred_depth_refinement][tx_type] <
                context_ptr->txt_ctrls.inter_th)
                return EB_TRUE;
        } else { // INTRA path
            if (context_ptr->txt_prob[pred_depth_refinement][tx_type] <
                context_ptr->txt_ctrls.intra_th)
                return EB_TRUE;
        }
    }
    return EB_FALSE;
}
#endif
uint8_t get_tx_type_group(ModeDecisionContext *        context_ptr,
                          ModeDecisionCandidateBuffer *candidate_buffer, EbBool only_dct_dct) {
    int tx_group = 1;
    if (!only_dct_dct) {
        if (candidate_buffer->candidate_ptr->cand_class == CAND_CLASS_0 ||
            candidate_buffer->candidate_ptr->cand_class == CAND_CLASS_3) {
            tx_group =
                (context_ptr->blk_geom->tx_width[context_ptr->tx_depth][context_ptr->txb_itr] <
                     16 ||
                 context_ptr->blk_geom->tx_height[context_ptr->tx_depth][context_ptr->txb_itr] < 16)
                ? context_ptr->txt_ctrls.txt_group_intra_lt_16x16
                : context_ptr->txt_ctrls.txt_group_intra_gt_eq_16x16;
        } else {
            tx_group =
                (context_ptr->blk_geom->tx_width[context_ptr->tx_depth][context_ptr->txb_itr] <
                     16 ||
                 context_ptr->blk_geom->tx_height[context_ptr->tx_depth][context_ptr->txb_itr] < 16)
                ? context_ptr->txt_ctrls.txt_group_inter_lt_16x16
                : context_ptr->txt_ctrls.txt_group_inter_gt_eq_16x16;
        }
    }
    return tx_group;
}
void tx_type_search(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                    ModeDecisionCandidateBuffer *candidate_buffer,
#if !FTR_REDUCE_TXT_BASED_ON_DISTORTION
                    uint32_t qindex, uint32_t *y_count_non_zero_coeffs, uint64_t *y_coeff_bits,
#else
                    uint32_t qindex, uint8_t tx_search_skip_flag, uint32_t *y_count_non_zero_coeffs,
                    uint64_t *y_coeff_bits,
#endif
                    uint64_t *y_full_distortion) {
    EbPictureBufferDesc *input_picture_ptr = context_ptr->hbd_mode_decision
        ? pcs_ptr->input_frame16bit
        : pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr;
    int32_t seg_qp = pcs_ptr->parent_pcs_ptr->frm_hdr.segmentation_params.segmentation_enabled
        ? pcs_ptr->parent_pcs_ptr->frm_hdr.segmentation_params
              .feature_data[context_ptr->blk_ptr->segment_id][SEG_LVL_ALT_Q]
        : 0;

    uint32_t full_lambda = context_ptr->hbd_mode_decision
        ? context_ptr->full_lambda_md[EB_10_BIT_MD]
        : context_ptr->full_lambda_md[EB_8_BIT_MD];
    TxSize   tx_size  = context_ptr->blk_geom->txsize[context_ptr->tx_depth][context_ptr->txb_itr];
    int32_t  is_inter = (candidate_buffer->candidate_ptr->type == INTER_MODE ||
                        candidate_buffer->candidate_ptr->use_intrabc)
         ? EB_TRUE
         : EB_FALSE;
    uint8_t  only_dct_dct = (context_ptr->md_staging_txt_level == 0);
#if FTR_REDUCE_TXT_BASED_ON_DISTORTION
    only_dct_dct = tx_search_skip_flag == 1 ? 1 : only_dct_dct;
#endif
    // Tunr OFF TXT search for disallowed cases
    // Do not turn ON TXT search beyond this point
    if (get_ext_tx_types(tx_size, is_inter, pcs_ptr->parent_pcs_ptr->frm_hdr.reduced_tx_set) == 1)
        only_dct_dct = 1;

    uint64_t        best_cost_tx_search = (uint64_t)~0;
    int32_t         tx_type;
    const TxSetType tx_set_type = get_ext_tx_set_type(
        tx_size, is_inter, pcs_ptr->parent_pcs_ptr->frm_hdr.reduced_tx_set);
    uint16_t txb_origin_x =
        context_ptr->blk_geom->tx_org_x[is_inter][context_ptr->tx_depth][context_ptr->txb_itr];
    uint16_t txb_origin_y =
        context_ptr->blk_geom->tx_org_y[is_inter][context_ptr->tx_depth][context_ptr->txb_itr];
    uint32_t txb_origin_index = txb_origin_x +
        (txb_origin_y * candidate_buffer->residual_ptr->stride_y);
    uint32_t input_txb_origin_index = (context_ptr->sb_origin_x + txb_origin_x +
                                       input_picture_ptr->origin_x) +
        ((context_ptr->sb_origin_y + txb_origin_y + input_picture_ptr->origin_y) *
         input_picture_ptr->stride_y);
    int32_t cropped_tx_width = MIN(
        context_ptr->blk_geom->tx_width[context_ptr->tx_depth][context_ptr->txb_itr],
        pcs_ptr->parent_pcs_ptr->aligned_width - (context_ptr->sb_origin_x + txb_origin_x));
    int32_t cropped_tx_height = MIN(
        context_ptr->blk_geom->tx_height[context_ptr->tx_depth][context_ptr->txb_itr],
        pcs_ptr->parent_pcs_ptr->aligned_height - (context_ptr->sb_origin_y + txb_origin_y));

    context_ptr->luma_txb_skip_context = 0;
    context_ptr->luma_dc_sign_context  = 0;
    if (!context_ptr->shut_skip_ctx_dc_sign_update)
        get_txb_ctx(pcs_ptr,
                    COMPONENT_LUMA,
                    context_ptr->full_loop_luma_dc_sign_level_coeff_neighbor_array,
                    context_ptr->sb_origin_x + txb_origin_x,
                    context_ptr->sb_origin_y + txb_origin_y,
                    context_ptr->blk_geom->bsize,
                    context_ptr->blk_geom->txsize[context_ptr->tx_depth][context_ptr->txb_itr],
                    &context_ptr->luma_txb_skip_context,
                    &context_ptr->luma_dc_sign_context);
    TxType best_tx_type = DCT_DCT;
#if !FTR_EOB_CUL_LEVEL
    uint8_t default_md_staging_spatial_sse_full_loop =
        context_ptr->md_staging_spatial_sse_full_loop_level;
#endif
    // local variables for all TX types
    uint16_t eob_txt[TX_TYPES]                                  = {0};
    int32_t  quantized_dc_txt[TX_TYPES]                         = {0};
    uint32_t y_count_non_zero_coeffs_txt[TX_TYPES]              = {0};
    uint64_t y_txb_coeff_bits_txt[TX_TYPES]                     = {0};
    uint64_t txb_full_distortion_txt[TX_TYPES][DIST_CALC_TOTAL] = {{0}};
    int      tx_type_tot_group = get_tx_type_group(context_ptr, candidate_buffer, only_dct_dct);
    for (int tx_type_group_idx = 0; tx_type_group_idx < tx_type_tot_group; ++tx_type_group_idx) {
#if OPT_TX_TYPE_SEARCH
        uint32_t best_tx_non_coeff = 64 * 64;
#endif
        for (int tx_type_idx = 0; tx_type_idx < TX_TYPES; ++tx_type_idx) {
#if FTR_ALIGN_SC_DETECOR
            if (pcs_ptr->parent_pcs_ptr->sc_class1)
#else
            if (pcs_ptr->parent_pcs_ptr->sc_content_detected)
#endif
                tx_type = tx_type_group_sc[tx_type_group_idx][tx_type_idx];
            else
                tx_type = tx_type_group[tx_type_group_idx][tx_type_idx];

            if (tx_type == INVALID_TX_TYPE)
                break;

            if (only_dct_dct && tx_type != DCT_DCT)
                continue;
#if !TUNE_REMOVE_TXT_STATS
            // Perform search selectively based on statistics (DCT_DCT always performed)
            if (context_ptr->txt_ctrls.use_stats && tx_type != DCT_DCT) {
                // Determine if current tx_type should be skipped based on statistics
                if (bypass_txt_based_on_stats(pcs_ptr,
                                              context_ptr,
                                              tx_type,
                                              tx_size,
                                              is_inter,
                                              y_count_non_zero_coeffs_txt[DCT_DCT]))
                    continue;
            }
#endif
            // Do not use temporary buffers when TXT is OFF
            EbPictureBufferDesc *recon_coeff_ptr = only_dct_dct
                ? candidate_buffer->recon_coeff_ptr
                : context_ptr->recon_coeff_ptr[tx_type];
            EbPictureBufferDesc *recon_ptr       = only_dct_dct ? candidate_buffer->recon_ptr
                                                                : context_ptr->recon_ptr[tx_type];

            context_ptr->three_quad_energy = 0;
            if (tx_type != DCT_DCT) {
                if (is_inter) {
                    TxSize          max_tx_size       = context_ptr->blk_geom->txsize[0][0];
                    const TxSetType tx_set_type_inter = get_ext_tx_set_type(
                        max_tx_size, is_inter, pcs_ptr->parent_pcs_ptr->frm_hdr.reduced_tx_set);
                    int32_t eset = get_ext_tx_set(
                        max_tx_size, is_inter, pcs_ptr->parent_pcs_ptr->frm_hdr.reduced_tx_set);
                    // eset == 0 should correspond to a set with only DCT_DCT and there
                    // is no need to send the tx_type
                    if (eset <= 0)
                        continue;
                    else if (av1_ext_tx_used[tx_set_type_inter][tx_type] == 0)
                        continue;
                    else if (context_ptr->blk_geom->tx_height[context_ptr->tx_depth]
                                                             [context_ptr->txb_itr] > 32 ||
                             context_ptr->blk_geom
                                     ->tx_width[context_ptr->tx_depth][context_ptr->txb_itr] > 32)
                        continue;
                }
                int32_t eset = get_ext_tx_set(
                    context_ptr->blk_geom->txsize[context_ptr->tx_depth][context_ptr->txb_itr],
                    is_inter,
                    pcs_ptr->parent_pcs_ptr->frm_hdr.reduced_tx_set);
                // eset == 0 should correspond to a set with only DCT_DCT and there
                // is no need to send the tx_type
                if (eset <= 0)
                    continue;
                else if (av1_ext_tx_used[tx_set_type][tx_type] == 0)
                    continue;
                else if (context_ptr->blk_geom
                                 ->tx_height[context_ptr->tx_depth][context_ptr->txb_itr] > 32 ||
                         context_ptr->blk_geom
                                 ->tx_width[context_ptr->tx_depth][context_ptr->txb_itr] > 32)
                    continue;
            }

            // For Inter blocks, transform type of chroma follows luma transfrom type
            if (is_inter)
                candidate_buffer->candidate_ptr->transform_type_uv = (context_ptr->txb_itr == 0)
                    ? candidate_buffer->candidate_ptr->transform_type[context_ptr->txb_itr]
                    : candidate_buffer->candidate_ptr->transform_type_uv;
#if FTR_REDUCE_MDS3_COMPLEXITY
            EB_TRANS_COEFF_SHAPE pf_shape = context_ptr->pf_ctrls.pf_shape;
            if (context_ptr->reduce_last_md_stage_candidate &&
                context_ptr->md_stage == MD_STAGE_3) {
                if (!candidate_buffer->candidate_ptr->block_has_coeff) {
                    pf_shape = N2_SHAPE;
                    if (context_ptr->mds0_best_idx == context_ptr->mds1_best_idx) {
                        if (candidate_buffer->candidate_ptr->cand_class !=
                            context_ptr->mds1_best_class_it) {
                            pf_shape = N4_SHAPE;
                        }
                    }
                }
            }
#endif
#if FTR_REDUCE_TXT_BASED_ON_DISTORTION
            if (!tx_search_skip_flag) {
#endif
                // Y: T Q i_q
                av1_estimate_transform(
                    &(((int16_t *)candidate_buffer->residual_ptr->buffer_y)[txb_origin_index]),
                    candidate_buffer->residual_ptr->stride_y,
                    &(((int32_t *)context_ptr->trans_quant_buffers_ptr->txb_trans_coeff2_nx2_n_ptr
                           ->buffer_y)[context_ptr->txb_1d_offset]),
                    NOT_USED_VALUE,
                    context_ptr->blk_geom->txsize[context_ptr->tx_depth][context_ptr->txb_itr],
                    &context_ptr->three_quad_energy,
                    context_ptr->hbd_mode_decision ? EB_10BIT : EB_8BIT,
                    tx_type,
                    PLANE_TYPE_Y,
#if FTR_REDUCE_MDS3_COMPLEXITY
                    pf_shape);
#else
                context_ptr->pf_ctrls.pf_shape);
#endif

                quantized_dc_txt[tx_type] = av1_quantize_inv_quantize(
                    pcs_ptr,
                    context_ptr,
                    &(((int32_t *)context_ptr->trans_quant_buffers_ptr->txb_trans_coeff2_nx2_n_ptr
                           ->buffer_y)[context_ptr->txb_1d_offset]),
                    NOT_USED_VALUE,
                    &(((int32_t *)context_ptr->residual_quant_coeff_ptr
                           ->buffer_y)[context_ptr->txb_1d_offset]),
                    &(((int32_t *)recon_coeff_ptr->buffer_y)[context_ptr->txb_1d_offset]),
                    qindex,
                    seg_qp,
                    context_ptr->blk_geom->tx_width[context_ptr->tx_depth][context_ptr->txb_itr],
                    context_ptr->blk_geom->tx_height[context_ptr->tx_depth][context_ptr->txb_itr],
                    context_ptr->blk_geom->txsize[context_ptr->tx_depth][context_ptr->txb_itr],
                    &eob_txt[tx_type],
                    &(y_count_non_zero_coeffs_txt[tx_type]),
                    COMPONENT_LUMA,
                    context_ptr->hbd_mode_decision ? EB_10BIT : EB_8BIT,
                    tx_type,
                    candidate_buffer,
                    context_ptr->luma_txb_skip_context,
                    context_ptr->luma_dc_sign_context,
                    candidate_buffer->candidate_ptr->pred_mode,
                    candidate_buffer->candidate_ptr->use_intrabc,
                    full_lambda,
                    EB_FALSE);
#if FTR_REDUCE_TXT_BASED_ON_DISTORTION
            }
#endif
            uint32_t y_has_coeff = y_count_non_zero_coeffs_txt[tx_type] > 0;

            // tx_type not equal to DCT_DCT and no coeff is not an acceptable option in AV1.
            if (y_has_coeff == 0 && tx_type != DCT_DCT)
                continue;

            // Perform T-1 if md_staging_spatial_sse_full_loop_level or  INTRA and tx_depth > 0 or
            if (context_ptr->md_staging_spatial_sse_full_loop_level ||
                (!is_inter && candidate_buffer->candidate_ptr->tx_depth)) {
                if (y_has_coeff)
                    inv_transform_recon_wrapper(
                        candidate_buffer->prediction_ptr->buffer_y,
                        txb_origin_index,
                        candidate_buffer->prediction_ptr->stride_y,
                        recon_ptr->buffer_y,
                        txb_origin_index,
                        candidate_buffer->recon_ptr->stride_y,
                        (int32_t *)recon_coeff_ptr->buffer_y,
                        context_ptr->txb_1d_offset,
                        context_ptr->hbd_mode_decision,
                        context_ptr->blk_geom->txsize[context_ptr->tx_depth][context_ptr->txb_itr],
                        tx_type,
                        PLANE_TYPE_Y,
                        (uint32_t)eob_txt[tx_type]);
                else
                    svt_av1_picture_copy(
                        candidate_buffer->prediction_ptr,
                        txb_origin_index,
                        0,
                        recon_ptr,
                        txb_origin_index,
                        0,
                        context_ptr->blk_geom
                            ->tx_width[context_ptr->tx_depth][context_ptr->txb_itr],
                        context_ptr->blk_geom
                            ->tx_height[context_ptr->tx_depth][context_ptr->txb_itr],
                        0,
                        0,
                        PICTURE_BUFFER_DESC_Y_FLAG,
                        context_ptr->hbd_mode_decision);

                EbSpatialFullDistType spatial_full_dist_type_fun = context_ptr->hbd_mode_decision
                    ? svt_full_distortion_kernel16_bits
                    : svt_spatial_full_distortion_kernel;
                txb_full_distortion_txt[tx_type][DIST_CALC_PREDICTION] = spatial_full_dist_type_fun(
                    input_picture_ptr->buffer_y,
                    input_txb_origin_index,
                    input_picture_ptr->stride_y,
                    candidate_buffer->prediction_ptr->buffer_y,
                    (int32_t)txb_origin_index,
                    candidate_buffer->prediction_ptr->stride_y,
                    cropped_tx_width,
                    cropped_tx_height);
                txb_full_distortion_txt[tx_type][DIST_CALC_RESIDUAL] = spatial_full_dist_type_fun(
                    input_picture_ptr->buffer_y,
                    input_txb_origin_index,
                    input_picture_ptr->stride_y,
                    recon_ptr->buffer_y,
                    (int32_t)txb_origin_index,
                    candidate_buffer->recon_ptr->stride_y,
                    cropped_tx_width,
                    cropped_tx_height);
                txb_full_distortion_txt[tx_type][DIST_CALC_PREDICTION] <<= 4;
                txb_full_distortion_txt[tx_type][DIST_CALC_RESIDUAL] <<= 4;
            } else {
                // LUMA DISTORTION
                picture_full_distortion32_bits(
                    context_ptr->trans_quant_buffers_ptr->txb_trans_coeff2_nx2_n_ptr,
                    context_ptr->txb_1d_offset,
                    0,
                    recon_coeff_ptr,
                    context_ptr->txb_1d_offset,
                    0,
                    context_ptr->blk_geom->tx_width[context_ptr->tx_depth][context_ptr->txb_itr],
                    context_ptr->blk_geom->tx_height[context_ptr->tx_depth][context_ptr->txb_itr],
                    NOT_USED_VALUE,
                    NOT_USED_VALUE,
                    txb_full_distortion_txt[tx_type],
                    NOT_USED_VALUE,
                    NOT_USED_VALUE,
                    y_count_non_zero_coeffs_txt[tx_type],
                    0,
                    0,
                    COMPONENT_LUMA);
                txb_full_distortion_txt[tx_type][DIST_CALC_RESIDUAL] +=
                    context_ptr->three_quad_energy;
                txb_full_distortion_txt[tx_type][DIST_CALC_PREDICTION] +=
                    context_ptr->three_quad_energy;
                //assert(context_ptr->three_quad_energy == 0 && context_ptr->cu_stats->size < 64);

                const int32_t shift =
                    (MAX_TX_SCALE -
                     av1_get_tx_scale_tab[context_ptr->blk_geom->txsize[context_ptr->tx_depth]
                                                                       [context_ptr->txb_itr]]) *
                    2;
                txb_full_distortion_txt[tx_type][DIST_CALC_RESIDUAL] = RIGHT_SIGNED_SHIFT(
                    txb_full_distortion_txt[tx_type][DIST_CALC_RESIDUAL], shift);
                txb_full_distortion_txt[tx_type][DIST_CALC_PREDICTION] = RIGHT_SIGNED_SHIFT(
                    txb_full_distortion_txt[tx_type][DIST_CALC_PREDICTION], shift);
            }
#if FTR_TXT_SKIP_RATE_EST
            // Do not perform rate estimation @ tx_type search if current tx_type dist is higher than best_cost
            uint64_t early_cost = RDCOST(
                full_lambda, 0, txb_full_distortion_txt[tx_type][DIST_CALC_RESIDUAL]);
            if (early_cost > best_cost_tx_search) {
                continue;
            }
#endif
            //LUMA-ONLY
            av1_txb_estimate_coeff_bits(
                context_ptr,
                0, //allow_update_cdf,
                NULL, //FRAME_CONTEXT *ec_ctx,
                pcs_ptr,
                candidate_buffer,
                context_ptr->txb_1d_offset,
                0,
                context_ptr->residual_quant_coeff_ptr,
                y_count_non_zero_coeffs_txt[tx_type],
                0,
                0,
                &(y_txb_coeff_bits_txt[tx_type]),
                &(y_txb_coeff_bits_txt[tx_type]),
                &(y_txb_coeff_bits_txt[tx_type]),
                context_ptr->blk_geom->txsize[context_ptr->tx_depth][context_ptr->txb_itr],
                context_ptr->blk_geom->txsize_uv[context_ptr->tx_depth][context_ptr->txb_itr],
                tx_type,
                candidate_buffer->candidate_ptr->transform_type_uv,
                COMPONENT_LUMA);
            uint64_t y_full_cost;
#if FIX_Y_COEFF_FLAG_UPDATE
            //TODO: fix cbf decision
            av1_txb_calc_cost_luma(txb_full_distortion_txt[tx_type],
                                   &(y_txb_coeff_bits_txt[tx_type]),
                                   &y_full_cost,
                                   full_lambda);
#else
            //TODO: fix cbf decision
            av1_txb_calc_cost_luma(context_ptr->luma_txb_skip_context,
                                   candidate_buffer->candidate_ptr,
                                   context_ptr->txb_itr,
                                   context_ptr->blk_geom->txsize[context_ptr->tx_depth][0],
                                   y_count_non_zero_coeffs_txt[tx_type],
                                   txb_full_distortion_txt[tx_type],
                                   &(y_txb_coeff_bits_txt[tx_type]),
                                   &y_full_cost,
                                   full_lambda);
#endif
            uint64_t cost = RDCOST(full_lambda,
                                   y_txb_coeff_bits_txt[tx_type],
                                   txb_full_distortion_txt[tx_type][DIST_CALC_RESIDUAL]);
            if (cost < best_cost_tx_search) {
                best_cost_tx_search = cost;
                best_tx_type        = tx_type;
#if OPT_TX_TYPE_SEARCH
                best_tx_non_coeff = y_count_non_zero_coeffs_txt[tx_type];
#endif
            }
#if OPT_TX_TYPE_SEARCH

#if TUNE_TXT_M9
            uint32_t coeff_th      = (context_ptr->early_txt_search_exit_level == 1) ? 4 : 16;
            uint32_t dist_err_unit = 100;
            uint32_t dist_err =
                context_ptr->blk_geom->txsize[context_ptr->tx_depth][context_ptr->txb_itr] *
                context_ptr->blk_geom->tx_height[context_ptr->tx_depth][context_ptr->txb_itr] *
                dist_err_unit;
            uint64_t cost_th = RDCOST(full_lambda, 1, dist_err);

            if (context_ptr->early_txt_search_exit_level &&
                (best_tx_non_coeff < coeff_th || best_cost_tx_search < cost_th)) {
#else
            uint32_t coeff_th = context_ptr->txt_exit_based_on_non_coeff_th;
            if (best_tx_non_coeff < coeff_th) {
#endif
                tx_type_idx       = TX_TYPES;
                tx_type_group_idx = tx_type_tot_group;
            }
#endif
        }
    }
#if !FTR_EOB_CUL_LEVEL
    context_ptr->md_staging_spatial_sse_full_loop_level = default_md_staging_spatial_sse_full_loop;
#endif
    //  Best Tx Type Pass
    candidate_buffer->candidate_ptr->transform_type[context_ptr->txb_itr] = best_tx_type;
    // update with best_tx_type data
    (*y_coeff_bits) += y_txb_coeff_bits_txt[best_tx_type];
    y_full_distortion[DIST_CALC_RESIDUAL] +=
        txb_full_distortion_txt[best_tx_type][DIST_CALC_RESIDUAL];
    y_full_distortion[DIST_CALC_PREDICTION] +=
        txb_full_distortion_txt[best_tx_type][DIST_CALC_PREDICTION];

    y_count_non_zero_coeffs[context_ptr->txb_itr] = y_count_non_zero_coeffs_txt[best_tx_type];
#if FIX_Y_COEFF_FLAG_UPDATE
    candidate_buffer->candidate_ptr->y_has_coeff |= ((y_count_non_zero_coeffs_txt[best_tx_type] > 0)
                                                     << context_ptr->txb_itr);
#endif
    candidate_buffer->candidate_ptr->quantized_dc[0][context_ptr->txb_itr] =
        quantized_dc_txt[best_tx_type];
    candidate_buffer->candidate_ptr->eob[0][context_ptr->txb_itr] = eob_txt[best_tx_type];

    // Do not copy when TXT is OFF
    // Data is already in candidate_buffer
    if (!only_dct_dct) {
        // copy best_tx_type data
        copy_txt_data(candidate_buffer, context_ptr, txb_origin_index, best_tx_type);
    }
    context_ptr->txb_1d_offset +=
        context_ptr->blk_geom->tx_width[context_ptr->tx_depth][context_ptr->txb_itr] *
        context_ptr->blk_geom->tx_height[context_ptr->tx_depth][context_ptr->txb_itr];

    // For Inter blocks, transform type of chroma follows luma transfrom type
    if (is_inter)
        candidate_buffer->candidate_ptr->transform_type_uv = (context_ptr->txb_itr == 0)
            ? candidate_buffer->candidate_ptr->transform_type[context_ptr->txb_itr]
            : candidate_buffer->candidate_ptr->transform_type_uv;
}

static INLINE int block_signals_txsize(BlockSize bsize) { return bsize > BLOCK_4X4; }

static INLINE int get_vartx_max_txsize(/*const MbModeInfo *xd,*/ BlockSize bsize, int plane) {
    /* if (xd->lossless[xd->mi[0]->segment_id]) return TX_4X4;*/
    const TxSize max_txsize = max_txsize_rect_lookup[bsize];
    if (plane == 0)
        return max_txsize; // luma
    return av1_get_adjusted_tx_size(max_txsize); // chroma
}

static INLINE int max_block_wide(const MacroBlockD *xd, BlockSize bsize, int plane) {
    int max_blocks_wide = block_size_wide[bsize];

    if (xd->mb_to_right_edge < 0)
        max_blocks_wide += gcc_right_shift(xd->mb_to_right_edge, 3 + !!plane);

    // Scale the width in the transform block unit.
    return max_blocks_wide >> tx_size_wide_log2[0];
}

static INLINE int max_block_high(const MacroBlockD *xd, BlockSize bsize, int plane) {
    int max_blocks_high = block_size_high[bsize];

    if (xd->mb_to_bottom_edge < 0)
        max_blocks_high += gcc_right_shift(xd->mb_to_bottom_edge, 3 + !!plane);

    // Scale the height in the transform block unit.
    return max_blocks_high >> tx_size_high_log2[0];
}

static INLINE void txfm_partition_update(TXFM_CONTEXT *above_ctx, TXFM_CONTEXT *left_ctx,
                                         TxSize tx_size, TxSize txb_size) {
    BlockSize bsize = txsize_to_bsize[txb_size];
    assert(bsize < BlockSizeS_ALL);
    int     bh  = mi_size_high[bsize];
    int     bw  = mi_size_wide[bsize];
    uint8_t txw = tx_size_wide[tx_size];
    uint8_t txh = tx_size_high[tx_size];
    int     i;
    for (i = 0; i < bh; ++i) left_ctx[i] = txh;
    for (i = 0; i < bw; ++i) above_ctx[i] = txw;
}

static INLINE TxSize get_sqr_tx_size(int tx_dim) {
    switch (tx_dim) {
    case 128:
    case 64: return TX_64X64; break;
    case 32: return TX_32X32; break;
    case 16: return TX_16X16; break;
    case 8: return TX_8X8; break;
    default: return TX_4X4;
    }
}
static INLINE int txfm_partition_context(TXFM_CONTEXT *above_ctx, TXFM_CONTEXT *left_ctx,
                                         BlockSize bsize, TxSize tx_size) {
    const uint8_t txw      = tx_size_wide[tx_size];
    const uint8_t txh      = tx_size_high[tx_size];
    const int     above    = *above_ctx < txw;
    const int     left     = *left_ctx < txh;
    int           category = TXFM_PARTITION_CONTEXTS;

    // dummy return, not used by others.
    if (tx_size == TX_4X4)
        return 0;

    TxSize max_tx_size = get_sqr_tx_size(AOMMAX(block_size_wide[bsize], block_size_high[bsize]));

    if (max_tx_size >= TX_8X8) {
        category = (txsize_sqr_up_map[tx_size] != max_tx_size && max_tx_size > TX_8X8) +
            (TX_SIZES - 1 - max_tx_size) * 2;
    }
    assert(category != TXFM_PARTITION_CONTEXTS);
    return category * 3 + above + left;
}

static uint64_t cost_tx_size_vartx(MacroBlockD *xd, const MbModeInfo *mbmi, TxSize tx_size,
                                   int depth, int blk_row, int blk_col,
                                   MdRateEstimationContext *md_rate_estimation_ptr) {
    uint64_t  bits            = 0;
    const int max_blocks_high = max_block_high(xd, mbmi->block_mi.sb_type, 0);
    const int max_blocks_wide = max_block_wide(xd, mbmi->block_mi.sb_type, 0);

    if (blk_row >= max_blocks_high || blk_col >= max_blocks_wide)
        return bits;

    if (depth == MAX_VARTX_DEPTH) {
        txfm_partition_update(
            xd->above_txfm_context + blk_col, xd->left_txfm_context + blk_row, tx_size, tx_size);

        return bits;
    }

    const int ctx = txfm_partition_context(xd->above_txfm_context + blk_col,
                                           xd->left_txfm_context + blk_row,
                                           mbmi->block_mi.sb_type,
                                           tx_size);

    const int write_txfm_partition = (tx_size ==
                                      tx_depth_to_tx_size[mbmi->tx_depth][mbmi->block_mi.sb_type]);

    if (write_txfm_partition) {
        bits += md_rate_estimation_ptr->txfm_partition_fac_bits[ctx][0];

        txfm_partition_update(
            xd->above_txfm_context + blk_col, xd->left_txfm_context + blk_row, tx_size, tx_size);

    } else {
        assert(tx_size < TX_SIZES_ALL);
        const TxSize sub_txs = sub_tx_size_map[tx_size];
        const int    bsw     = tx_size_wide_unit[sub_txs];
        const int    bsh     = tx_size_high_unit[sub_txs];

        bits += md_rate_estimation_ptr->txfm_partition_fac_bits[ctx][1];
        if (sub_txs == TX_4X4) {
            txfm_partition_update(xd->above_txfm_context + blk_col,
                                  xd->left_txfm_context + blk_row,
                                  sub_txs,
                                  tx_size);

            return bits;
        }

        assert(bsw > 0 && bsh > 0);
        for (int row = 0; row < tx_size_high_unit[tx_size]; row += bsh)
            for (int col = 0; col < tx_size_wide_unit[tx_size]; col += bsw) {
                int offsetr = blk_row + row;
                int offsetc = blk_col + col;
                bits += cost_tx_size_vartx(
                    xd, mbmi, sub_txs, depth + 1, offsetr, offsetc, md_rate_estimation_ptr);
            }
    }
    return bits;
}

static INLINE void set_txfm_ctx(TXFM_CONTEXT *txfm_ctx, uint8_t txs, int len) {
    int i;
    for (i = 0; i < len; ++i) txfm_ctx[i] = txs;
}

static INLINE void set_txfm_ctxs(TxSize tx_size, int n8_w, int n8_h, int skip,
                                 const MacroBlockD *xd) {
    uint8_t bw = tx_size_wide[tx_size];
    uint8_t bh = tx_size_high[tx_size];

    if (skip) {
        bw = n8_w * MI_SIZE;
        bh = n8_h * MI_SIZE;
    }

    set_txfm_ctx(xd->above_txfm_context, bw, n8_w);
    set_txfm_ctx(xd->left_txfm_context, bh, n8_h);
}

static INLINE int tx_size_to_depth(TxSize tx_size, BlockSize bsize) {
    TxSize ctx_size = max_txsize_rect_lookup[bsize];
    int    depth    = 0;
    while (tx_size != ctx_size) {
        depth++;
        ctx_size = sub_tx_size_map[ctx_size];
        assert(depth <= MAX_TX_DEPTH);
    }
    return depth;
}

#define BLOCK_SIZES_ALL 22

int is_inter_block(const BlockModeInfo *bloc_mi);

// Returns a context number for the given MB prediction signal
// The mode info data structure has a one element border above and to the
// left of the entries corresponding to real blocks.
// The prediction flags in these dummy entries are initialized to 0.
static INLINE int get_tx_size_context(const MacroBlockD *xd) {
    const ModeInfo *        mi          = xd->mi[0];
    const MbModeInfo *      mbmi        = &mi->mbmi;
    const MbModeInfo *const above_mbmi  = xd->above_mbmi;
    const MbModeInfo *const left_mbmi   = xd->left_mbmi;
    const TxSize            max_tx_size = max_txsize_rect_lookup[mbmi->block_mi.sb_type];
    const int               max_tx_wide = tx_size_wide[max_tx_size];
    const int               max_tx_high = tx_size_high[max_tx_size];
    const int               has_above   = xd->up_available;
    const int               has_left    = xd->left_available;

    int above = xd->above_txfm_context[0] >= max_tx_wide;
    int left  = xd->left_txfm_context[0] >= max_tx_high;

    if (has_above)
        if (is_inter_block(&above_mbmi->block_mi))
            above = block_size_wide[above_mbmi->block_mi.sb_type] >= max_tx_wide;

    if (has_left)
        if (is_inter_block(&left_mbmi->block_mi))
            left = block_size_high[left_mbmi->block_mi.sb_type] >= max_tx_high;

    if (has_above && has_left)
        return (above + left);
    else if (has_above)
        return above;
    else if (has_left)
        return left;
    else
        return 0;
}

static uint64_t cost_selected_tx_size(const MacroBlockD *      xd,
                                      MdRateEstimationContext *md_rate_estimation_ptr) {
    const ModeInfo *const   mi    = xd->mi[0];
    const MbModeInfo *const mbmi  = &mi->mbmi;
    const BlockSize         bsize = mbmi->block_mi.sb_type;
    uint64_t                bits  = 0;

    if (block_signals_txsize(bsize)) {
        const TxSize tx_size     = mbmi->tx_size;
        const int    tx_size_ctx = get_tx_size_context(xd);
        assert(bsize < BlockSizeS_ALL);
        const int     depth       = tx_size_to_depth(tx_size, bsize);
        const int32_t tx_size_cat = bsize_to_tx_size_cat(bsize);
        bits += md_rate_estimation_ptr->tx_size_fac_bits[tx_size_cat][tx_size_ctx][depth];
    }

    return bits;
}

static uint64_t tx_size_bits(MdRateEstimationContext *md_rate_estimation_ptr, MacroBlockD *xd,
                             const MbModeInfo *mbmi, TxMode tx_mode, BlockSize bsize,
                             uint8_t skip) {
    uint64_t bits = 0;

    int is_inter_tx = is_inter_block(&mbmi->block_mi) || is_intrabc_block(&mbmi->block_mi);
    if (tx_mode == TX_MODE_SELECT && block_signals_txsize(bsize) &&
        !(is_inter_tx && skip) /*&& !xd->lossless[segment_id]*/) {
        if (is_inter_tx) { // This implies skip flag is 0.
            const TxSize max_tx_size = get_vartx_max_txsize(/*xd,*/ bsize, 0);
            const int    txbh        = tx_size_high_unit[max_tx_size];
            const int    txbw        = tx_size_wide_unit[max_tx_size];
            const int    width       = block_size_wide[bsize] >> tx_size_wide_log2[0];
            const int    height      = block_size_high[bsize] >> tx_size_high_log2[0];
            int          idx, idy;
            for (idy = 0; idy < height; idy += txbh)
                for (idx = 0; idx < width; idx += txbw)
                    bits += cost_tx_size_vartx(
                        xd, mbmi, max_tx_size, 0, idy, idx, md_rate_estimation_ptr);
        } else {
            bits += cost_selected_tx_size(xd, md_rate_estimation_ptr);
            set_txfm_ctxs(mbmi->tx_size, xd->n8_w, xd->n8_h, 0, xd);
        }
    } else {
        set_txfm_ctxs(
            mbmi->tx_size, xd->n8_w, xd->n8_h, skip && is_inter_block(&mbmi->block_mi), xd);
    }
    return bits;
}

void set_mi_row_col(PictureControlSet *pcs_ptr, MacroBlockD *xd, TileInfo *tile, int mi_row, int bh,
                    int mi_col, int bw, uint32_t mi_stride, int mi_rows, int mi_cols);

uint64_t estimate_tx_size_bits(PictureControlSet *pcsPtr, ModeDecisionContext *context_ptr,
                               ModeDecisionCandidate *candidate_ptr, EbBool skip_flag,
                               uint32_t blk_origin_x, uint32_t blk_origin_y, BlkStruct *blk_ptr,
                               const BlockGeom *blk_geom, NeighborArrayUnit *txfm_context_array,
                               uint8_t tx_depth, MdRateEstimationContext *md_rate_estimation_ptr) {
    uint32_t txfm_context_left_index  = get_neighbor_array_unit_left_index(txfm_context_array,
                                                                          blk_origin_y);
    uint32_t txfm_context_above_index = get_neighbor_array_unit_top_index(txfm_context_array,
                                                                          blk_origin_x);

    TxMode        tx_mode   = pcsPtr->parent_pcs_ptr->frm_hdr.tx_mode;
#if !OPT_INIT_XD_2
    Av1Common *   cm        = pcsPtr->parent_pcs_ptr->av1_cm;
#endif
    MacroBlockD * xd        = blk_ptr->av1xd;
#if !OPT_INIT_XD_2
    TileInfo *    tile      = &xd->tile;
    int32_t       mi_row    = blk_origin_y >> MI_SIZE_LOG2;
    int32_t       mi_col    = blk_origin_x >> MI_SIZE_LOG2;
#endif
    BlockSize     bsize     = blk_geom->bsize;
#if !OPT_INIT_XD_2
    const int32_t bw        = mi_size_wide[bsize];
    const int32_t bh        = mi_size_high[bsize];
    uint32_t      mi_stride = pcsPtr->mi_stride;

    set_mi_row_col(pcsPtr, xd, tile, mi_row, bh, mi_col, bw, mi_stride, cm->mi_rows, cm->mi_cols);
#endif
    MbModeInfo *mbmi = &xd->mi[0]->mbmi;

    svt_memcpy(context_ptr->above_txfm_context,
               &(txfm_context_array->top_array[txfm_context_above_index]),
               (blk_geom->bwidth >> MI_SIZE_LOG2) * sizeof(TXFM_CONTEXT));
    svt_memcpy(context_ptr->left_txfm_context,
               &(txfm_context_array->left_array[txfm_context_left_index]),
               (blk_geom->bheight >> MI_SIZE_LOG2) * sizeof(TXFM_CONTEXT));

    xd->above_txfm_context = context_ptr->above_txfm_context;
    xd->left_txfm_context  = context_ptr->left_txfm_context;

    mbmi->tx_size               = blk_geom->txsize[tx_depth][0];
    mbmi->block_mi.sb_type      = blk_geom->bsize;
    mbmi->block_mi.use_intrabc  = candidate_ptr->use_intrabc;
    mbmi->block_mi.ref_frame[0] = candidate_ptr->ref_frame_type;
    mbmi->tx_depth              = tx_depth;

    uint64_t bits = tx_size_bits(md_rate_estimation_ptr, xd, mbmi, tx_mode, bsize, skip_flag);

    return bits;
}

uint64_t get_tx_size_bits(ModeDecisionCandidateBuffer *candidateBuffer,
                          ModeDecisionContext *context_ptr, PictureControlSet *pcs_ptr,
                          uint8_t tx_depth, EbBool block_has_coeff) {
    return estimate_tx_size_bits(pcs_ptr,
                                 context_ptr,
                                 candidateBuffer->candidate_ptr,
                                 block_has_coeff ? 0 : 1,
                                 context_ptr->blk_origin_x,
                                 context_ptr->blk_origin_y,
                                 context_ptr->blk_ptr,
                                 context_ptr->blk_geom,
                                 context_ptr->txfm_context_array,
                                 tx_depth,
                                 context_ptr->md_rate_estimation_ptr);
}
void first_pass_init_tx_candidate_buffer(ModeDecisionCandidateBuffer *candidate_buffer,
                                         ModeDecisionContext *context_ptr, uint8_t end_tx_depth) {
    uint32_t block_index = context_ptr->blk_geom->origin_x +
        (context_ptr->blk_geom->origin_y * context_ptr->sb_size);
    if (end_tx_depth) {
        svt_memcpy(context_ptr->candidate_buffer_tx_depth_1->candidate_ptr,
                   candidate_buffer->candidate_ptr,
                   sizeof(ModeDecisionCandidate));
        svt_memcpy(context_ptr->candidate_buffer_tx_depth_2->candidate_ptr,
                   candidate_buffer->candidate_ptr,
                   sizeof(ModeDecisionCandidate));
        if (context_ptr->hbd_mode_decision) {
            // Copy residual to tx_depth_1 candidate_buffer
            {
                int16_t *src = &(
                    ((int16_t *)candidate_buffer->residual_ptr->buffer_y)[block_index]);
                int16_t *dst = &(((int16_t *)context_ptr->candidate_buffer_tx_depth_1->residual_ptr
                                      ->buffer_y)[block_index]);

                for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                    svt_memcpy(dst, src, context_ptr->blk_geom->bwidth << 1);
                    src += candidate_buffer->residual_ptr->stride_y;
                    dst += context_ptr->candidate_buffer_tx_depth_1->residual_ptr->stride_y;
                }
            }
        } else {
            // Copy residual to tx_depth_1 candidate_buffer
            {
                int16_t *src = &(
                    ((int16_t *)candidate_buffer->residual_ptr->buffer_y)[block_index]);
                int16_t *dst = &(((int16_t *)context_ptr->candidate_buffer_tx_depth_1->residual_ptr
                                      ->buffer_y)[block_index]);

                for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                    svt_memcpy(dst, src, context_ptr->blk_geom->bwidth << 1);
                    src += candidate_buffer->residual_ptr->stride_y;
                    dst += context_ptr->candidate_buffer_tx_depth_1->residual_ptr->stride_y;
                }
            }
        }
    }
    if (end_tx_depth == 2) {
        if (context_ptr->hbd_mode_decision) {
            // Copy residual to tx_depth_1 candidate_buffer
            {
                int16_t *src = &(
                    ((int16_t *)candidate_buffer->residual_ptr->buffer_y)[block_index]);
                int16_t *dst = &(((int16_t *)context_ptr->candidate_buffer_tx_depth_2->residual_ptr
                                      ->buffer_y)[block_index]);

                for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                    svt_memcpy(dst, src, context_ptr->blk_geom->bwidth << 1);
                    src += candidate_buffer->residual_ptr->stride_y;
                    dst += context_ptr->candidate_buffer_tx_depth_2->residual_ptr->stride_y;
                }
            }
        } else {
            // Copy residual to tx_depth_2 candidate_buffer
            {
                int16_t *src = &(
                    ((int16_t *)candidate_buffer->residual_ptr->buffer_y)[block_index]);
                int16_t *dst = &(((int16_t *)context_ptr->candidate_buffer_tx_depth_2->residual_ptr
                                      ->buffer_y)[block_index]);

                for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                    svt_memcpy(dst, src, context_ptr->blk_geom->bwidth << 1);
                    src += candidate_buffer->residual_ptr->stride_y;
                    dst += context_ptr->candidate_buffer_tx_depth_2->residual_ptr->stride_y;
                }
            }
        }
    }
}
void init_tx_candidate_buffer(ModeDecisionCandidateBuffer *candidate_buffer,
                              ModeDecisionContext *context_ptr, uint8_t end_tx_depth) {
    uint32_t block_index = context_ptr->blk_geom->origin_x +
        (context_ptr->blk_geom->origin_y * context_ptr->sb_size);
    if (end_tx_depth) {
        svt_memcpy(context_ptr->candidate_buffer_tx_depth_1->candidate_ptr,
                   candidate_buffer->candidate_ptr,
                   sizeof(ModeDecisionCandidate));
        svt_memcpy(context_ptr->candidate_buffer_tx_depth_2->candidate_ptr,
                   candidate_buffer->candidate_ptr,
                   sizeof(ModeDecisionCandidate));
        if (context_ptr->hbd_mode_decision) {
            // Copy pred to tx_depth_1 candidate_buffer
            {
                uint16_t *src = &(
                    ((uint16_t *)candidate_buffer->prediction_ptr->buffer_y)[block_index]);
                uint16_t *dst = &(((uint16_t *)context_ptr->candidate_buffer_tx_depth_1
                                       ->prediction_ptr->buffer_y)[block_index]);
                for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                    svt_memcpy(dst, src, context_ptr->blk_geom->bwidth * sizeof(uint16_t));
                    src += candidate_buffer->prediction_ptr->stride_y;
                    dst += context_ptr->candidate_buffer_tx_depth_1->prediction_ptr->stride_y;
                }
            }
            // Copy residual to tx_depth_1 candidate_buffer
            {
                int16_t *src = &(
                    ((int16_t *)candidate_buffer->residual_ptr->buffer_y)[block_index]);
                int16_t *dst = &(((int16_t *)context_ptr->candidate_buffer_tx_depth_1->residual_ptr
                                      ->buffer_y)[block_index]);

                for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                    svt_memcpy(dst, src, context_ptr->blk_geom->bwidth << 1);
                    src += candidate_buffer->residual_ptr->stride_y;
                    dst += context_ptr->candidate_buffer_tx_depth_1->residual_ptr->stride_y;
                }
            }
        } else {
            // Copy pred to tx_depth_1 candidate_buffer
            {
                EbByte src = &(candidate_buffer->prediction_ptr->buffer_y[block_index]);
                EbByte dst = &(context_ptr->candidate_buffer_tx_depth_1->prediction_ptr
                                   ->buffer_y[block_index]);
                for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                    svt_memcpy(dst, src, context_ptr->blk_geom->bwidth);
                    src += candidate_buffer->prediction_ptr->stride_y;
                    dst += context_ptr->candidate_buffer_tx_depth_1->prediction_ptr->stride_y;
                }
            }
            // Copy residual to tx_depth_1 candidate_buffer
            {
                int16_t *src = &(
                    ((int16_t *)candidate_buffer->residual_ptr->buffer_y)[block_index]);
                int16_t *dst = &(((int16_t *)context_ptr->candidate_buffer_tx_depth_1->residual_ptr
                                      ->buffer_y)[block_index]);

                for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                    svt_memcpy(dst, src, context_ptr->blk_geom->bwidth << 1);
                    src += candidate_buffer->residual_ptr->stride_y;
                    dst += context_ptr->candidate_buffer_tx_depth_1->residual_ptr->stride_y;
                }
            }
        }
    }
    if (end_tx_depth == 2) {
        if (context_ptr->hbd_mode_decision) {
            // Copy pred to tx_depth_1 candidate_buffer
            {
                uint16_t *src = &(
                    ((uint16_t *)candidate_buffer->prediction_ptr->buffer_y)[block_index]);
                uint16_t *dst = &(((uint16_t *)context_ptr->candidate_buffer_tx_depth_2
                                       ->prediction_ptr->buffer_y)[block_index]);

                for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                    svt_memcpy(dst, src, context_ptr->blk_geom->bwidth * sizeof(uint16_t));
                    src += candidate_buffer->prediction_ptr->stride_y;
                    dst += context_ptr->candidate_buffer_tx_depth_2->prediction_ptr->stride_y;
                }
            }
            // Copy residual to tx_depth_1 candidate_buffer
            {
                int16_t *src = &(
                    ((int16_t *)candidate_buffer->residual_ptr->buffer_y)[block_index]);
                int16_t *dst = &(((int16_t *)context_ptr->candidate_buffer_tx_depth_2->residual_ptr
                                      ->buffer_y)[block_index]);

                for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                    svt_memcpy(dst, src, context_ptr->blk_geom->bwidth << 1);
                    src += candidate_buffer->residual_ptr->stride_y;
                    dst += context_ptr->candidate_buffer_tx_depth_2->residual_ptr->stride_y;
                }
            }
        } else {
            // Copy pred to tx_depth_2 candidate_buffer
            {
                EbByte src = &(candidate_buffer->prediction_ptr->buffer_y[block_index]);
                EbByte dst = &(context_ptr->candidate_buffer_tx_depth_2->prediction_ptr
                                   ->buffer_y[block_index]);
                for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                    svt_memcpy(dst, src, context_ptr->blk_geom->bwidth);
                    src += candidate_buffer->prediction_ptr->stride_y;
                    dst += context_ptr->candidate_buffer_tx_depth_2->prediction_ptr->stride_y;
                }
            }
            // Copy residual to tx_depth_2 candidate_buffer
            {
                int16_t *src = &(
                    ((int16_t *)candidate_buffer->residual_ptr->buffer_y)[block_index]);
                int16_t *dst = &(((int16_t *)context_ptr->candidate_buffer_tx_depth_2->residual_ptr
                                      ->buffer_y)[block_index]);

                for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                    svt_memcpy(dst, src, context_ptr->blk_geom->bwidth << 1);
                    src += candidate_buffer->residual_ptr->stride_y;
                    dst += context_ptr->candidate_buffer_tx_depth_2->residual_ptr->stride_y;
                }
            }
        }
    }
}

void update_tx_candidate_buffer(ModeDecisionCandidateBuffer *candidate_buffer,
                                ModeDecisionContext *context_ptr, uint8_t best_tx_depth) {
    uint32_t block_index = context_ptr->blk_geom->origin_x +
        (context_ptr->blk_geom->origin_y * context_ptr->sb_size);
    if (best_tx_depth == 1) {
        // Copy depth 1 mode/type/eob ..
        svt_memcpy(candidate_buffer->candidate_ptr,
                   context_ptr->candidate_buffer_tx_depth_1->candidate_ptr,
                   sizeof(ModeDecisionCandidate));
        // Copy depth 1 pred
        if (context_ptr->hbd_mode_decision) {
            uint16_t *src = &(((uint16_t *)context_ptr->candidate_buffer_tx_depth_1->prediction_ptr
                                   ->buffer_y)[block_index]);
            uint16_t *dst = &(
                ((uint16_t *)candidate_buffer->prediction_ptr->buffer_y)[block_index]);
            for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                svt_memcpy(dst, src, context_ptr->blk_geom->bwidth * sizeof(uint16_t));
                src += context_ptr->candidate_buffer_tx_depth_1->prediction_ptr->stride_y;
                dst += candidate_buffer->prediction_ptr->stride_y;
            }
        } else {
            EbByte src = &(
                context_ptr->candidate_buffer_tx_depth_1->prediction_ptr->buffer_y[block_index]);
            EbByte dst = &(candidate_buffer->prediction_ptr->buffer_y[block_index]);
            for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                svt_memcpy(dst, src, context_ptr->blk_geom->bwidth);
                src += context_ptr->candidate_buffer_tx_depth_1->prediction_ptr->stride_y;
                dst += candidate_buffer->prediction_ptr->stride_y;
            }
        }
        // Copy depth 1 recon coeff
        svt_memcpy(candidate_buffer->recon_coeff_ptr->buffer_y,
                   context_ptr->candidate_buffer_tx_depth_1->recon_coeff_ptr->buffer_y,
                   (context_ptr->blk_geom->bwidth * context_ptr->blk_geom->bheight << 2));
    }
    if (best_tx_depth == 2) {
        // Copy depth 2 mode/type/eob ..
        svt_memcpy(candidate_buffer->candidate_ptr,
                   context_ptr->candidate_buffer_tx_depth_2->candidate_ptr,
                   sizeof(ModeDecisionCandidate));
        // Copy depth 2 pred
        if (context_ptr->hbd_mode_decision) {
            uint16_t *src = &(((uint16_t *)context_ptr->candidate_buffer_tx_depth_2->prediction_ptr
                                   ->buffer_y)[block_index]);
            uint16_t *dst = &(
                ((uint16_t *)candidate_buffer->prediction_ptr->buffer_y)[block_index]);
            for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                svt_memcpy(dst, src, context_ptr->blk_geom->bwidth * sizeof(uint16_t));
                src += context_ptr->candidate_buffer_tx_depth_2->prediction_ptr->stride_y;
                dst += candidate_buffer->prediction_ptr->stride_y;
            }
        } else {
            EbByte src = &(
                context_ptr->candidate_buffer_tx_depth_2->prediction_ptr->buffer_y[block_index]);
            EbByte dst = &(candidate_buffer->prediction_ptr->buffer_y[block_index]);
            for (int i = 0; i < context_ptr->blk_geom->bheight; i++) {
                svt_memcpy(dst, src, context_ptr->blk_geom->bwidth);
                src += context_ptr->candidate_buffer_tx_depth_2->prediction_ptr->stride_y;
                dst += candidate_buffer->prediction_ptr->stride_y;
            }
        }
        // Copy depth 2 recon coeff
        svt_memcpy(candidate_buffer->recon_coeff_ptr->buffer_y,
                   context_ptr->candidate_buffer_tx_depth_2->recon_coeff_ptr->buffer_y,
                   (context_ptr->blk_geom->bwidth * context_ptr->blk_geom->bheight << 2));
    }
}
// TX path for first pass
void first_pass_perform_tx_partitioning(ModeDecisionCandidateBuffer *candidate_buffer,
                                        ModeDecisionContext *        context_ptr,
                                        PictureControlSet *pcs_ptr, uint8_t start_tx_depth,
                                        uint8_t end_tx_depth, uint64_t *y_coeff_bits,
                                        uint64_t *y_full_distortion) {
    uint32_t             full_lambda       = context_ptr->hbd_mode_decision
                          ? context_ptr->full_lambda_md[EB_10_BIT_MD]
                          : context_ptr->full_lambda_md[EB_8_BIT_MD];
    EbPictureBufferDesc *input_picture_ptr = context_ptr->hbd_mode_decision
        ? pcs_ptr->input_frame16bit
        : pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr;
    int32_t              is_inter          = (candidate_buffer->candidate_ptr->type == INTER_MODE ||
                        candidate_buffer->candidate_ptr->use_intrabc)
                              ? EB_TRUE
                              : EB_FALSE;

    uint8_t  best_tx_depth     = 0;
    uint64_t best_cost_search  = (uint64_t)~0;
    uint8_t  is_best_has_coeff = 1;
    first_pass_init_tx_candidate_buffer(candidate_buffer, context_ptr, end_tx_depth);
    // Transform Depth Loop
    for (context_ptr->tx_depth = start_tx_depth; context_ptr->tx_depth <= end_tx_depth;
         context_ptr->tx_depth++) {
        if (pcs_ptr->parent_pcs_ptr->tx_size_early_exit) {
            if (!is_best_has_coeff)
                continue;
        }
        ModeDecisionCandidateBuffer *tx_candidate_buffer = (context_ptr->tx_depth == 0)
            ? candidate_buffer
            : (context_ptr->tx_depth == 1) ? context_ptr->candidate_buffer_tx_depth_1
                                           : context_ptr->candidate_buffer_tx_depth_2;
        tx_candidate_buffer->candidate_ptr->tx_depth     = context_ptr->tx_depth;
        // Initialize TU Split
        uint64_t tx_y_coeff_bits                       = 0;
        uint64_t tx_y_full_distortion[DIST_CALC_TOTAL] = {0};

        context_ptr->txb_1d_offset                      = 0;
        context_ptr->three_quad_energy                  = 0;
        tx_candidate_buffer->candidate_ptr->y_has_coeff = 0;

        uint16_t txb_count = context_ptr->blk_geom->txb_count[context_ptr->tx_depth];

        for (context_ptr->txb_itr = 0; context_ptr->txb_itr < txb_count; context_ptr->txb_itr++) {
            uint16_t tx_org_x = context_ptr->blk_geom->tx_org_x[is_inter][context_ptr->tx_depth]
                                                               [context_ptr->txb_itr];
            uint16_t tx_org_y = context_ptr->blk_geom->tx_org_y[is_inter][context_ptr->tx_depth]
                                                               [context_ptr->txb_itr];

            uint32_t input_txb_origin_index = (context_ptr->sb_origin_x + tx_org_x +
                                               input_picture_ptr->origin_x) +
                ((context_ptr->sb_origin_y + tx_org_y + input_picture_ptr->origin_y) *
                 input_picture_ptr->stride_y);

            // Y Prediction

            if (!is_inter) {
                av1_first_pass_intra_luma_prediction(input_picture_ptr,
                                                     input_txb_origin_index,
                                                     context_ptr,
                                                     pcs_ptr,
                                                     tx_candidate_buffer);
            }

        } // Transform Loop
        if (end_tx_depth) {
            const uint64_t tx_size_bit = pcs_ptr->parent_pcs_ptr->frm_hdr.tx_mode == TX_MODE_SELECT
                ? get_tx_size_bits(
                      tx_candidate_buffer, context_ptr, pcs_ptr, context_ptr->tx_depth, EB_FALSE)
                : 0;

            const uint64_t cost = RDCOST(full_lambda,
                                         tx_y_coeff_bits + tx_size_bit,
                                         tx_y_full_distortion[DIST_CALC_RESIDUAL]);
            if (cost < best_cost_search) {
                best_cost_search                      = cost;
                best_tx_depth                         = context_ptr->tx_depth;
                is_best_has_coeff                     = EB_FALSE;
                y_full_distortion[DIST_CALC_RESIDUAL] = tx_y_full_distortion[DIST_CALC_RESIDUAL];
                y_full_distortion[DIST_CALC_PREDICTION] =
                    tx_y_full_distortion[DIST_CALC_PREDICTION];
                *y_coeff_bits = tx_y_coeff_bits;
            }
        } else {
            y_full_distortion[DIST_CALC_RESIDUAL]   = tx_y_full_distortion[DIST_CALC_RESIDUAL];
            y_full_distortion[DIST_CALC_PREDICTION] = tx_y_full_distortion[DIST_CALC_PREDICTION];
            *y_coeff_bits                           = tx_y_coeff_bits;
        }

    } // Transform Depth Loop

    update_tx_candidate_buffer(candidate_buffer, context_ptr, best_tx_depth);
}
void perform_tx_partitioning(ModeDecisionCandidateBuffer *candidate_buffer,
                             ModeDecisionContext *context_ptr, PictureControlSet *pcs_ptr,
                             uint8_t start_tx_depth, uint8_t end_tx_depth, uint32_t qindex,
                             uint32_t *y_count_non_zero_coeffs, uint64_t *y_coeff_bits,
                             uint64_t *y_full_distortion) {
    uint32_t             full_lambda       = context_ptr->hbd_mode_decision
                          ? context_ptr->full_lambda_md[EB_10_BIT_MD]
                          : context_ptr->full_lambda_md[EB_8_BIT_MD];
    EbPictureBufferDesc *input_picture_ptr = context_ptr->hbd_mode_decision
        ? pcs_ptr->input_frame16bit
        : pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr;
    int32_t              is_inter          = (candidate_buffer->candidate_ptr->type == INTER_MODE ||
                        candidate_buffer->candidate_ptr->use_intrabc)
                              ? EB_TRUE
                              : EB_FALSE;

    uint8_t  best_tx_depth     = 0;
    uint64_t best_cost_search  = (uint64_t)~0;
    uint8_t  is_best_has_coeff = 1;
    init_tx_candidate_buffer(candidate_buffer, context_ptr, end_tx_depth);
    // Transform Depth Loop
    for (context_ptr->tx_depth = start_tx_depth; context_ptr->tx_depth <= end_tx_depth;
         context_ptr->tx_depth++) {
        if (pcs_ptr->parent_pcs_ptr->tx_size_early_exit) {
            if (!is_best_has_coeff)
                continue;
        }
        tx_reset_neighbor_arrays(pcs_ptr, context_ptr, is_inter, context_ptr->tx_depth);
        ModeDecisionCandidateBuffer *tx_candidate_buffer = (context_ptr->tx_depth == 0)
            ? candidate_buffer
            : (context_ptr->tx_depth == 1) ? context_ptr->candidate_buffer_tx_depth_1
                                           : context_ptr->candidate_buffer_tx_depth_2;
        tx_candidate_buffer->candidate_ptr->tx_depth     = context_ptr->tx_depth;

        tx_initialize_neighbor_arrays(pcs_ptr, context_ptr, is_inter);

        // Initialize TU Split
        uint32_t tx_y_count_non_zero_coeffs[MAX_NUM_OF_TU_PER_CU];
        uint64_t tx_y_coeff_bits                       = 0;
        uint64_t tx_y_full_distortion[DIST_CALC_TOTAL] = {0};

        context_ptr->txb_1d_offset                      = 0;
        context_ptr->three_quad_energy                  = 0;
        tx_candidate_buffer->candidate_ptr->y_has_coeff = 0;

        uint16_t txb_count = context_ptr->blk_geom->txb_count[context_ptr->tx_depth];

        uint32_t block_has_coeff = EB_FALSE;
        for (context_ptr->txb_itr = 0; context_ptr->txb_itr < txb_count; context_ptr->txb_itr++) {
            uint16_t tx_org_x = context_ptr->blk_geom->tx_org_x[is_inter][context_ptr->tx_depth]
                                                               [context_ptr->txb_itr];
            uint16_t tx_org_y = context_ptr->blk_geom->tx_org_y[is_inter][context_ptr->tx_depth]
                                                               [context_ptr->txb_itr];
            uint32_t txb_origin_index = tx_org_x +
                (tx_org_y * tx_candidate_buffer->residual_ptr->stride_y);
            uint32_t input_txb_origin_index = (context_ptr->sb_origin_x + tx_org_x +
                                               input_picture_ptr->origin_x) +
                ((context_ptr->sb_origin_y + tx_org_y + input_picture_ptr->origin_y) *
                 input_picture_ptr->stride_y);

            // Y Prediction

            if (!is_inter) {
                // This check assumes no txs search @ a previous md_stage()
                if (context_ptr->tx_depth)
                    av1_intra_luma_prediction(context_ptr, pcs_ptr, tx_candidate_buffer);

                // Y Residual
                residual_kernel(
                    input_picture_ptr->buffer_y,
                    input_txb_origin_index,
                    input_picture_ptr->stride_y,
                    tx_candidate_buffer->prediction_ptr->buffer_y,
                    txb_origin_index,
                    tx_candidate_buffer->prediction_ptr->stride_y,
                    (int16_t *)tx_candidate_buffer->residual_ptr->buffer_y,
                    txb_origin_index,
                    tx_candidate_buffer->residual_ptr->stride_y,
                    context_ptr->hbd_mode_decision,
                    context_ptr->blk_geom->tx_width[context_ptr->tx_depth][context_ptr->txb_itr],
                    context_ptr->blk_geom->tx_height[context_ptr->tx_depth][context_ptr->txb_itr]);
            }
#if FTR_REDUCE_TXT_BASED_ON_DISTORTION
            uint8_t tx_search_skip_flag = 0;
            if (context_ptr->bypass_tx_search_when_zcoef)
                if (context_ptr->md_stage == MD_STAGE_3)
                    if (context_ptr->mds0_best_idx == context_ptr->mds1_best_idx)
                        tx_search_skip_flag = candidate_buffer->candidate_ptr->block_has_coeff == 0
                            ? 1
                            : 0;
#endif
            tx_type_search(pcs_ptr,
                           context_ptr,
                           tx_candidate_buffer,
                           qindex,
#if FTR_REDUCE_TXT_BASED_ON_DISTORTION
                           tx_search_skip_flag,
#endif
                           &(tx_y_count_non_zero_coeffs[0]),
                           &tx_y_coeff_bits,
                           &tx_y_full_distortion[0]);

            uint32_t y_has_coeff = tx_y_count_non_zero_coeffs[context_ptr->txb_itr] > 0;

            tx_update_neighbor_arrays(pcs_ptr, context_ptr, tx_candidate_buffer, is_inter);

            if (y_has_coeff)
                block_has_coeff = EB_TRUE;

            uint64_t current_tx_cost = RDCOST(
                full_lambda, tx_y_coeff_bits, tx_y_full_distortion[DIST_CALC_RESIDUAL]);
            if (current_tx_cost > best_cost_search)
                break;

        } // Transform Loop

        if (end_tx_depth) {
            const uint64_t tx_size_bit = pcs_ptr->parent_pcs_ptr->frm_hdr.tx_mode == TX_MODE_SELECT
                ? get_tx_size_bits(tx_candidate_buffer,
                                   context_ptr,
                                   pcs_ptr,
                                   context_ptr->tx_depth,
                                   block_has_coeff)
                : 0;

            const uint64_t cost = RDCOST(full_lambda,
                                         tx_y_coeff_bits + tx_size_bit,
                                         tx_y_full_distortion[DIST_CALC_RESIDUAL]);
            if (cost < best_cost_search) {
                best_cost_search                      = cost;
                best_tx_depth                         = context_ptr->tx_depth;
                is_best_has_coeff                     = block_has_coeff;
                y_full_distortion[DIST_CALC_RESIDUAL] = tx_y_full_distortion[DIST_CALC_RESIDUAL];
                y_full_distortion[DIST_CALC_PREDICTION] =
                    tx_y_full_distortion[DIST_CALC_PREDICTION];
                *y_coeff_bits = tx_y_coeff_bits;
                for (context_ptr->txb_itr = 0; context_ptr->txb_itr < txb_count;
                     context_ptr->txb_itr++) {
                    y_count_non_zero_coeffs[context_ptr->txb_itr] =
                        tx_y_count_non_zero_coeffs[context_ptr->txb_itr];
                }
            }
        } else {
            y_full_distortion[DIST_CALC_RESIDUAL]   = tx_y_full_distortion[DIST_CALC_RESIDUAL];
            y_full_distortion[DIST_CALC_PREDICTION] = tx_y_full_distortion[DIST_CALC_PREDICTION];
            *y_coeff_bits                           = tx_y_coeff_bits;
            for (context_ptr->txb_itr = 0; context_ptr->txb_itr < txb_count;
                 context_ptr->txb_itr++) {
                y_count_non_zero_coeffs[context_ptr->txb_itr] =
                    tx_y_count_non_zero_coeffs[context_ptr->txb_itr];
            }
        }
    } // Transform Depth Loop

    update_tx_candidate_buffer(candidate_buffer, context_ptr, best_tx_depth);
}
#if !CLN_NSQ_AND_STATS
// Stats table for TXS
uint8_t m0_intra_txs_depth_1_cycles_reduction_stats[6 /*depth*/][3 /*pred-depth delta*/]
                                                   [2 /*sq/nsq*/][2 /*freq band*/] = {
                                                       {// DEPTH 0
                                                        {
                                                            // pred - 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 1
                                                        {
                                                            // pred - 1
                                                            {1, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {15, 1} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {5, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {4, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 2
                                                        {
                                                            // pred - 1
                                                            {22, 2}, // SQ:  [0%,10%], [10%,100%]
                                                            {53, 21} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {14, 2}, // SQ:  [0%,10%], [10%,100%]
                                                            {21, 8} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {1, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {3, 2} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 3
                                                        {
                                                            // pred - 1
                                                            {15, 18}, // SQ:  [0%,10%], [10%,100%]
                                                            {38, 195} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {20, 13}, // SQ:  [0%,10%], [10%,100%]
                                                            {23, 91} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {4, 3}, // SQ:  [0%,10%], [10%,100%]
                                                            {5, 22} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 4
                                                        {
                                                            // pred - 1
                                                            {2, 52}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {4, 62}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {1, 25}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 5
                                                        {
                                                            // pred - 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }}};
uint8_t m0_intra_txs_depth_2_cycles_reduction_stats[6 /*depth*/][3 /*pred-depth delta*/]
                                                   [2 /*sq/nsq*/][2 /*freq band*/] = {
                                                       {// DEPTH 0
                                                        {
                                                            // pred - 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 1
                                                        {
                                                            // pred - 1
                                                            {1, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {5, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {1, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {2, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 2
                                                        {
                                                            // pred - 1
                                                            {10, 1}, // SQ:  [0%,10%], [10%,100%]
                                                            {25, 9} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {3, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {7, 2} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {1, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {1, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 3
                                                        {
                                                            // pred - 1
                                                            {9, 15}, // SQ:  [0%,10%], [10%,100%]
                                                            {14, 77} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {6, 4}, // SQ:  [0%,10%], [10%,100%]
                                                            {7, 23} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {1, 1}, // SQ:  [0%,10%], [10%,100%]
                                                            {1, 6} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 4
                                                        {
                                                            // pred - 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 5
                                                        {
                                                            // pred - 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }}};

// INTER TXS tables
uint8_t m0_inter_txs_depth_1_cycles_reduction_stats[6 /*depth*/][3 /*pred-depth delta*/]
                                                   [2 /*sq/nsq*/][2 /*freq band*/] = {
                                                       {// DEPTH 0
                                                        {
                                                            // pred - 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 1
                                                        {
                                                            // pred - 1
                                                            {24, 1}, // SQ:  [0%,10%], [10%,100%]
                                                            {21, 4} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {30, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {15, 2} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {3, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {3, 1} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 2
                                                        {
                                                            // pred - 1
                                                            {25, 11}, // SQ:  [0%,10%], [10%,100%]
                                                            {44, 17} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {24, 5}, // SQ:  [0%,10%], [10%,100%]
                                                            {26, 8} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {6, 2}, // SQ:  [0%,10%], [10%,100%]
                                                            {7, 4} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 3
                                                        {
                                                            // pred - 1
                                                            {28, 21}, // SQ:  [0%,10%], [10%,100%]
                                                            {52, 119} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {50, 15}, // SQ:  [0%,10%], [10%,100%]
                                                            {46, 54} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {12, 3}, // SQ:  [0%,10%], [10%,100%]
                                                            {9, 10} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 4
                                                        {
                                                            // pred - 1
                                                            {5, 36}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {10, 31}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {3, 6}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 5
                                                        {
                                                            // pred - 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }}};
uint8_t m0_inter_txs_depth_2_cycles_reduction_stats[6 /*depth*/][3 /*pred-depth delta*/]
                                                   [2 /*sq/nsq*/][2 /*freq band*/] = {
                                                       {// DEPTH 0
                                                        {
                                                            // pred - 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 1
                                                        {
                                                            // pred - 1
                                                            {4, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {4, 1} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {5, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {3, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 2
                                                        {
                                                            // pred - 1
                                                            {6, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {10, 3} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {4, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {4, 1} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {1, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {1, 1} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 3
                                                        {
                                                            // pred - 1
                                                            {11, 5}, // SQ:  [0%,10%], [10%,100%]
                                                            {19, 70} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {7, 1}, // SQ:  [0%,10%], [10%,100%]
                                                            {9, 26} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {1, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {1, 5} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 4
                                                        {
                                                            // pred - 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }},
                                                       {// DEPTH 5
                                                        {
                                                            // pred - 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred depth
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        },
                                                        {
                                                            // pred + 1
                                                            {0, 0}, // SQ:  [0%,10%], [10%,100%]
                                                            {0, 0} // NSQ: [0%,10%], [10%,100%]
                                                        }}};
/*
 * Update the end TX depth based on statistics.
 *
 * Inputs:
 * end_tx_depth - corresponds to the current max. TX depth that TXS could use.
 * is_inter - whether the current block uses inter or intra prediction.
 *
 * Returns:
 * Nothing, but updates end_tx_depth to the new maximum depth that should be evaluated, as determined by the statistics
 * of previously evaluated blocks.
 */
void bypass_txs_based_on_stats(ModeDecisionContext *context_ptr, uint8_t *end_tx_depth,
                               EbBool is_inter) {
    int8_t pred_depth_refinement =
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds].pred_depth_refinement;
    // adjust the recorded pred depth refinement to avoid array access issues
    pred_depth_refinement = MIN(pred_depth_refinement, 1);
    pred_depth_refinement = MAX(pred_depth_refinement, -1);
    pred_depth_refinement++;
    uint8_t is_nsq      = context_ptr->blk_geom->shape == PART_N ? 0 : 1;
    uint8_t freq_band   = context_ptr->sb_class <= SB_CLASS_15 ? 1 : 0;
    uint8_t sq_size_idx = 7 - (uint8_t)log2f_32(context_ptr->blk_geom->sq_size);

    // Bypass TXS for INTRA
    if (!is_inter) {
        // Check TXS depth 1 first; if depth 1 becomes disallowed, do not allow depth 2
        *end_tx_depth =
            m0_intra_txs_depth_1_cycles_reduction_stats[sq_size_idx][pred_depth_refinement][is_nsq]
                                                       [freq_band] <
                context_ptr->txs_cycles_red_ctrls.intra_th
            ? 0
            : *end_tx_depth;

        if (*end_tx_depth == 2) {
            *end_tx_depth =
                m0_intra_txs_depth_2_cycles_reduction_stats[sq_size_idx][pred_depth_refinement]
                                                           [is_nsq][freq_band] <
                    context_ptr->txs_cycles_red_ctrls.intra_th
                ? 1
                : *end_tx_depth;
        }
    } else { // Bypass TXS for INTER
        // Check TXS depth 1 first; if depth 1 becomes disallowed, do not allow depth 2
        *end_tx_depth =
            m0_inter_txs_depth_1_cycles_reduction_stats[sq_size_idx][pred_depth_refinement][is_nsq]
                                                       [freq_band] <
                context_ptr->txs_cycles_red_ctrls.inter_th
            ? 0
            : *end_tx_depth;

        if (*end_tx_depth == 2) {
            *end_tx_depth =
                m0_inter_txs_depth_2_cycles_reduction_stats[sq_size_idx][pred_depth_refinement]
                                                           [is_nsq][freq_band] <
                    context_ptr->txs_cycles_red_ctrls.inter_th
                ? 1
                : *end_tx_depth;
        }
    }
}
#endif
void full_loop_core(PictureControlSet *pcs_ptr, SuperBlock *sb_ptr, BlkStruct *blk_ptr,
                    ModeDecisionContext *context_ptr, ModeDecisionCandidateBuffer *candidate_buffer,
                    ModeDecisionCandidate *candidate_ptr, EbPictureBufferDesc *input_picture_ptr,
                    uint32_t input_origin_index, uint32_t input_cb_origin_in_index,
                    uint32_t blk_origin_index, uint32_t blk_chroma_origin_index) {
    uint64_t y_full_distortion[DIST_CALC_TOTAL];
    uint32_t count_non_zero_coeffs[3][MAX_NUM_OF_TU_PER_CU];

    uint64_t cb_full_distortion[DIST_CALC_TOTAL];
    uint64_t cr_full_distortion[DIST_CALC_TOTAL];

    uint64_t y_coeff_bits;
    uint64_t cb_coeff_bits = 0;
    uint64_t cr_coeff_bits = 0;
    uint32_t full_lambda   = context_ptr->hbd_mode_decision
          ? context_ptr->full_lambda_md[EB_10_BIT_MD]
          : context_ptr->full_lambda_md[EB_8_BIT_MD];
    int32_t  is_inter      = (candidate_buffer->candidate_ptr->type == INTER_MODE ||
                        candidate_buffer->candidate_ptr->use_intrabc)
              ? EB_TRUE
              : EB_FALSE;
    // initialize TU Split
    y_full_distortion[DIST_CALC_RESIDUAL]   = 0;
    y_full_distortion[DIST_CALC_PREDICTION] = 0;
    y_coeff_bits                            = 0;

    candidate_ptr->full_distortion = 0;
#if !FTR_EOB_CUL_LEVEL
    memset(candidate_ptr->eob[0], 0, sizeof(uint16_t));
    memset(candidate_ptr->eob[1], 0, sizeof(uint16_t));
    memset(candidate_ptr->eob[2], 0, sizeof(uint16_t));
#endif
    // Set Skip Flag
    candidate_ptr->skip_flag = EB_FALSE;

    if (candidate_ptr->type != INTRA_MODE) {
        if (context_ptr->md_staging_perform_inter_pred) {
            svt_product_prediction_fun_table[candidate_ptr->type](
                context_ptr->hbd_mode_decision, context_ptr, pcs_ptr, candidate_buffer);
        }
    } else if (context_ptr->md_staging_skip_full_chroma == EB_FALSE) {
        if (context_ptr->blk_geom->has_uv && context_ptr->chroma_level <= CHROMA_MODE_1) {
            // Cb/Cr Prediction
            if (context_ptr->md_staging_perform_intra_chroma_pred) {
                context_ptr->uv_intra_comp_only = EB_TRUE;
                svt_product_prediction_fun_table[candidate_buffer->candidate_ptr->use_intrabc
                                                     ? INTER_MODE
                                                     : candidate_ptr->type](
                    context_ptr->hbd_mode_decision, context_ptr, pcs_ptr, candidate_buffer);
            }
        }
    }
    // Initialize luma CBF
    candidate_ptr->y_has_coeff = 0;
    candidate_ptr->u_has_coeff = 0;
    candidate_ptr->v_has_coeff = 0;
#if !FTR_EOB_CUL_LEVEL
    // Initialize tx type
    for (int tu_index = 0; tu_index < MAX_TXB_COUNT; tu_index++)
        candidate_ptr->transform_type[tu_index] = DCT_DCT;
#endif
    uint8_t start_tx_depth = 0;
    uint8_t end_tx_depth   = 0;
    if (context_ptr->md_tx_size_search_mode == 0) {
        start_tx_depth = end_tx_depth = 0;
    } else if (context_ptr->md_staging_tx_size_mode == 0) {
        start_tx_depth = end_tx_depth = candidate_buffer->candidate_ptr->tx_depth;
    } else {
        // end_tx_depth set to zero for blocks which go beyond the picture boundaries
        if ((context_ptr->sb_origin_x + context_ptr->blk_geom->origin_x +
                     context_ptr->blk_geom->bwidth <=
                 pcs_ptr->parent_pcs_ptr->scs_ptr->seq_header.max_frame_width &&
             context_ptr->sb_origin_y + context_ptr->blk_geom->origin_y +
                     context_ptr->blk_geom->bheight <=
                 pcs_ptr->parent_pcs_ptr->scs_ptr->seq_header.max_frame_height))
            end_tx_depth = get_end_tx_depth(context_ptr->blk_geom->bsize);
        else
            end_tx_depth = 0;
#if !CLN_NSQ_AND_STATS
        // Bypass TXS based on statistics
        if (context_ptr->txs_cycles_red_ctrls.enabled && end_tx_depth != 0) {
            // Update the end TX depth based on statistics
            bypass_txs_based_on_stats(context_ptr, &end_tx_depth, is_inter);
        }
#endif
    }
    if (is_inter && (context_ptr->md_staging_tx_size_level))
        end_tx_depth = MIN(1, end_tx_depth);
    //Y Residual: residual for INTRA is computed inside the TU loop
    if (is_inter)
        //Y Residual
        residual_kernel(input_picture_ptr->buffer_y,
                        input_origin_index,
                        input_picture_ptr->stride_y,
                        candidate_buffer->prediction_ptr->buffer_y,
                        blk_origin_index,
                        candidate_buffer->prediction_ptr->stride_y,
                        (int16_t *)candidate_buffer->residual_ptr->buffer_y,
                        blk_origin_index,
                        candidate_buffer->residual_ptr->stride_y,
                        context_ptr->hbd_mode_decision,
                        context_ptr->blk_geom->bwidth,
                        context_ptr->blk_geom->bheight);

    perform_tx_partitioning(candidate_buffer,
                            context_ptr,
                            pcs_ptr,
                            start_tx_depth,
                            end_tx_depth,
                            context_ptr->blk_ptr->qindex,
                            &(*count_non_zero_coeffs[0]),
                            &y_coeff_bits,
                            &y_full_distortion[0]);

    //CHROMA

    cb_full_distortion[DIST_CALC_RESIDUAL]   = 0;
    cr_full_distortion[DIST_CALC_RESIDUAL]   = 0;
    cb_full_distortion[DIST_CALC_PREDICTION] = 0;
    cr_full_distortion[DIST_CALC_PREDICTION] = 0;

    cb_coeff_bits = 0;
    cr_coeff_bits = 0;

    // FullLoop and TU search
    uint16_t cb_qindex = context_ptr->qp_index;
    uint16_t cr_qindex = cb_qindex;
    if (context_ptr->md_staging_skip_full_chroma == EB_FALSE) {
        if (context_ptr->blk_geom->has_uv && context_ptr->chroma_level <= CHROMA_MODE_1) {
            //Cb Residual
            residual_kernel(input_picture_ptr->buffer_cb,
                            input_cb_origin_in_index,
                            input_picture_ptr->stride_cb,
                            candidate_buffer->prediction_ptr->buffer_cb,
                            blk_chroma_origin_index,
                            candidate_buffer->prediction_ptr->stride_cb,
                            (int16_t *)candidate_buffer->residual_ptr->buffer_cb,
                            blk_chroma_origin_index,
                            candidate_buffer->residual_ptr->stride_cb,
                            context_ptr->hbd_mode_decision,
                            context_ptr->blk_geom->bwidth_uv,
                            context_ptr->blk_geom->bheight_uv);

            //Cr Residual
            residual_kernel(input_picture_ptr->buffer_cr,
                            input_cb_origin_in_index,
                            input_picture_ptr->stride_cr,
                            candidate_buffer->prediction_ptr->buffer_cr,
                            blk_chroma_origin_index,
                            candidate_buffer->prediction_ptr->stride_cr,
                            (int16_t *)candidate_buffer->residual_ptr->buffer_cr,
                            blk_chroma_origin_index,
                            candidate_buffer->residual_ptr->stride_cr,
                            context_ptr->hbd_mode_decision,
                            context_ptr->blk_geom->bwidth_uv,
                            context_ptr->blk_geom->bheight_uv);
        }
        EbBool cfl_performed = EB_FALSE;
        if (!is_inter)
            if (candidate_buffer->candidate_ptr->intra_chroma_mode == UV_CFL_PRED) {
                cfl_performed = EB_TRUE;
                // If mode is CFL:
                // 1: recon the Luma
                // 2: Form the pred_buf_q3
                // 3: Loop over alphas and find the best or choose DC
                // 4: Recalculate the residual for chroma
                cfl_prediction(pcs_ptr,
                               candidate_buffer,
                               sb_ptr,
                               context_ptr,
                               input_picture_ptr,
                               input_cb_origin_in_index,
                               blk_chroma_origin_index);
            }

        if (context_ptr->blk_geom->has_uv && context_ptr->chroma_level <= CHROMA_MODE_1) {
            full_loop_r(sb_ptr,
                        candidate_buffer,
                        context_ptr,
                        input_picture_ptr,
                        pcs_ptr,
                        PICTURE_BUFFER_DESC_CHROMA_MASK,
                        cb_qindex,
                        cr_qindex,
                        &(*count_non_zero_coeffs[1]),
                        &(*count_non_zero_coeffs[2]));

            cu_full_distortion_fast_txb_mode_r(sb_ptr,
                                               candidate_buffer,
                                               context_ptr,
                                               candidate_ptr,
                                               pcs_ptr,
                                               input_picture_ptr,
                                               cb_full_distortion,
                                               cr_full_distortion,
                                               count_non_zero_coeffs,
                                               COMPONENT_CHROMA,
                                               &cb_coeff_bits,
                                               &cr_coeff_bits,
                                               1);
        }

        // Check independant chroma vs. cfl
        if (!is_inter)
            if (candidate_ptr->palette_info == NULL ||
                candidate_ptr->palette_info->pmi.palette_size[0] == 0)
                if (context_ptr->blk_geom->has_uv && context_ptr->chroma_level == CHROMA_MODE_0)
                    if (cfl_performed)
                        check_best_indepedant_cfl(pcs_ptr,
                                                  input_picture_ptr,
                                                  context_ptr,
                                                  input_cb_origin_in_index,
                                                  blk_chroma_origin_index,
                                                  candidate_buffer,
                                                  (uint8_t)cb_qindex,
                                                  (uint8_t)cr_qindex,
                                                  cb_full_distortion,
                                                  cr_full_distortion,
                                                  &cb_coeff_bits,
                                                  &cr_coeff_bits);
    }

    candidate_ptr->block_has_coeff = (candidate_ptr->y_has_coeff | candidate_ptr->u_has_coeff |
                                      candidate_ptr->v_has_coeff)
        ? EB_TRUE
        : EB_FALSE;

    //ALL PLANE
    svt_av1_product_full_cost_func_table[candidate_ptr->type](pcs_ptr,
                                                              context_ptr,
                                                              candidate_buffer,
                                                              blk_ptr,
                                                              y_full_distortion,
                                                              cb_full_distortion,
                                                              cr_full_distortion,
                                                              full_lambda,
                                                              &y_coeff_bits,
                                                              &cb_coeff_bits,
                                                              &cr_coeff_bits,
                                                              context_ptr->blk_geom->bsize);
    uint16_t txb_count =
        context_ptr->blk_geom->txb_count[candidate_buffer->candidate_ptr->tx_depth];
    candidate_ptr->count_non_zero_coeffs = 0;
    for (uint8_t txb_itr = 0; txb_itr < txb_count; txb_itr++)
        candidate_ptr->count_non_zero_coeffs += count_non_zero_coeffs[0][txb_itr];
}

static void md_stage_1(PictureControlSet *pcs_ptr, SuperBlock *sb_ptr, BlkStruct *blk_ptr,
                       ModeDecisionContext *context_ptr, EbPictureBufferDesc *input_picture_ptr,
                       uint32_t input_origin_index, uint32_t input_cb_origin_in_index,
                       uint32_t blk_origin_index, uint32_t blk_chroma_origin_index) {
    ModeDecisionCandidateBuffer **candidate_buffer_ptr_array_base =
        context_ptr->candidate_buffer_ptr_array;
    ModeDecisionCandidateBuffer **candidate_buffer_ptr_array = &(
        candidate_buffer_ptr_array_base[0]);

    // Set MD Staging full_loop_core settings
    context_ptr->md_staging_tx_size_mode     = 0;
    context_ptr->md_staging_txt_level        = 0;
    context_ptr->md_staging_skip_full_chroma = EB_TRUE;
    context_ptr->md_staging_skip_rdoq        = EB_TRUE;

    context_ptr->md_staging_spatial_sse_full_loop_level = EB_FALSE;

    for (uint32_t full_loop_candidate_index = 0;
         full_loop_candidate_index < context_ptr->md_stage_1_count[context_ptr->target_class];
         ++full_loop_candidate_index) {
        uint32_t cand_index =
            context_ptr->cand_buff_indices[context_ptr->target_class][full_loop_candidate_index];
        ModeDecisionCandidateBuffer *candidate_buffer = candidate_buffer_ptr_array[cand_index];
        ModeDecisionCandidate *      candidate_ptr    = candidate_buffer->candidate_ptr;
        context_ptr->md_staging_perform_inter_pred    = (context_ptr->interpolation_search_level ==
                                                      IFS_MDS1)
               ? EB_TRUE
               : EB_FALSE;
        context_ptr->md_staging_skip_interpolation_search =
            (context_ptr->interpolation_search_level == IFS_MDS1) ? EB_FALSE : EB_TRUE;
        context_ptr->md_staging_skip_chroma_pred          = EB_TRUE;
        candidate_buffer->candidate_ptr->interp_filters   = 0;
        context_ptr->md_staging_perform_intra_chroma_pred = EB_FALSE;
        full_loop_core(pcs_ptr,
                       sb_ptr,
                       blk_ptr,
                       context_ptr,
                       candidate_buffer,
                       candidate_ptr,
                       input_picture_ptr,
                       input_origin_index,
                       input_cb_origin_in_index,
                       blk_origin_index,
                       blk_chroma_origin_index);
    }
}

static void md_stage_2(PictureControlSet *pcs_ptr, SuperBlock *sb_ptr, BlkStruct *blk_ptr,
                       ModeDecisionContext *context_ptr, EbPictureBufferDesc *input_picture_ptr,
                       uint32_t input_origin_index, uint32_t input_cb_origin_in_index,
                       uint32_t blk_origin_index, uint32_t blk_chroma_origin_index) {
    ModeDecisionCandidateBuffer **candidate_buffer_ptr_array_base =
        context_ptr->candidate_buffer_ptr_array;
    ModeDecisionCandidateBuffer **candidate_buffer_ptr_array = &(
        candidate_buffer_ptr_array_base[0]);

    // Set MD Staging full_loop_core settings
    for (uint32_t fullLoopCandidateIndex = 0;
         fullLoopCandidateIndex < context_ptr->md_stage_2_count[context_ptr->target_class];
         ++fullLoopCandidateIndex) {
        uint32_t candidateIndex =
            context_ptr->cand_buff_indices[context_ptr->target_class][fullLoopCandidateIndex];
        ModeDecisionCandidateBuffer *candidate_buffer = candidate_buffer_ptr_array[candidateIndex];
        ModeDecisionCandidate *      candidate_ptr    = candidate_buffer->candidate_ptr;

        context_ptr->md_staging_tx_size_mode       = (candidate_ptr->cand_class == CAND_CLASS_0 ||
                                                candidate_ptr->cand_class == CAND_CLASS_3)
                  ? 0
                  : (context_ptr->md_staging_tx_size_level) ? 1
                                                            : 0;
        context_ptr->md_staging_txt_level          = (candidate_ptr->cand_class == CAND_CLASS_0 ||
                                             candidate_ptr->cand_class == CAND_CLASS_3)
                     ? context_ptr->txt_ctrls.enabled
                     : 0;
        context_ptr->md_staging_skip_rdoq          = (candidate_ptr->cand_class == CAND_CLASS_0 ||
                                             candidate_ptr->cand_class == CAND_CLASS_3)
                     ? EB_TRUE
                     : EB_FALSE;
        context_ptr->md_staging_skip_full_chroma   = EB_TRUE;
        context_ptr->md_staging_perform_inter_pred = (context_ptr->interpolation_search_level ==
                                                      IFS_MDS2)
            ? EB_TRUE
            : EB_FALSE;
        context_ptr->md_staging_skip_interpolation_search =
            (context_ptr->interpolation_search_level == IFS_MDS2) ? EB_FALSE : EB_TRUE;
        context_ptr->md_staging_skip_chroma_pred = EB_TRUE;

        context_ptr->md_staging_spatial_sse_full_loop_level =
            context_ptr->spatial_sse_full_loop_level;

        context_ptr->md_staging_perform_intra_chroma_pred = EB_FALSE;

        full_loop_core(pcs_ptr,
                       sb_ptr,
                       blk_ptr,
                       context_ptr,
                       candidate_buffer,
                       candidate_ptr,
                       input_picture_ptr,
                       input_origin_index,
                       input_cb_origin_in_index,
                       blk_origin_index,
                       blk_chroma_origin_index);
    }
}

void update_intra_chroma_mode(ModeDecisionContext *  context_ptr,
                              ModeDecisionCandidate *candidate_ptr, PictureControlSet *pcs_ptr) {
    int32_t is_inter = (candidate_ptr->type == INTER_MODE || candidate_ptr->use_intrabc) ? EB_TRUE
                                                                                         : EB_FALSE;
    if (context_ptr->blk_geom->sq_size < 128) {
        if (context_ptr->blk_geom->has_uv) {
            if (!is_inter) {
                if (candidate_ptr->palette_info == NULL ||
                    candidate_ptr->palette_info->pmi.palette_size[0] == 0) {
                    uint32_t intra_chroma_mode;
                    int32_t  angle_delta;
                    uint8_t  is_directional_chroma_mode_flag;
                    if (((context_ptr->best_inter_cost *
                          context_ptr->chroma_at_last_md_stage_cfl_th) <
                         (context_ptr->best_intra_cost * 100))) {
                        intra_chroma_mode =
                            context_ptr->best_uv_mode[candidate_ptr->intra_luma_mode]
                                                     [MAX_ANGLE_DELTA +
                                                      candidate_ptr->angle_delta[PLANE_TYPE_Y]];
                        angle_delta =
                            context_ptr->best_uv_angle[candidate_ptr->intra_luma_mode]
                                                      [MAX_ANGLE_DELTA +
                                                       candidate_ptr->angle_delta[PLANE_TYPE_Y]];
                        is_directional_chroma_mode_flag = (uint8_t)av1_is_directional_mode((
                            PredictionMode)(
                            context_ptr->best_uv_mode[candidate_ptr->intra_luma_mode]
                                                     [MAX_ANGLE_DELTA +
                                                      candidate_ptr->angle_delta[PLANE_TYPE_Y]]));
                    } else {
                        intra_chroma_mode = candidate_ptr->intra_chroma_mode != UV_CFL_PRED
                            ? context_ptr->best_uv_mode[candidate_ptr->intra_luma_mode]
                                                       [MAX_ANGLE_DELTA +
                                                        candidate_ptr->angle_delta[PLANE_TYPE_Y]]
                            : UV_CFL_PRED;
                        angle_delta       = candidate_ptr->intra_chroma_mode != UV_CFL_PRED
                                  ? context_ptr->best_uv_angle[candidate_ptr->intra_luma_mode]
                                                        [MAX_ANGLE_DELTA +
                                                         candidate_ptr->angle_delta[PLANE_TYPE_Y]]
                                  : 0;
                        is_directional_chroma_mode_flag = candidate_ptr->intra_chroma_mode !=
                                UV_CFL_PRED
                            ? (uint8_t)av1_is_directional_mode((PredictionMode)(
                                  context_ptr
                                      ->best_uv_mode[candidate_ptr->intra_luma_mode]
                                                    [MAX_ANGLE_DELTA +
                                                     candidate_ptr->angle_delta[PLANE_TYPE_Y]]))
                            : 0;
                    }
                    // If CFL OFF or not applicable, and intra_chroma_mode used @ md_stage_0() (first stage intra_mode)
                    // and the best independant intra mode (final stage intra_mode) are not matching then the chroma pred
                    // should be re-performed using best independant chroma pred
                    if (candidate_ptr->intra_chroma_mode != UV_CFL_PRED)
                        if (candidate_ptr->intra_chroma_mode != intra_chroma_mode ||
                            candidate_ptr->angle_delta[PLANE_TYPE_UV] != angle_delta) {
                            // Set to TRUE to redo INTRA CHROMA compensation
                            context_ptr->md_staging_perform_intra_chroma_pred = EB_TRUE;
                            // Update fast_chroma_rate
                            candidate_ptr->fast_chroma_rate =
                                context_ptr
                                    ->fast_chroma_rate[candidate_ptr->intra_luma_mode]
                                                      [MAX_ANGLE_DELTA +
                                                       candidate_ptr->angle_delta[PLANE_TYPE_Y]];
                            // Update intra_chroma_mode
                            candidate_ptr->intra_chroma_mode          = intra_chroma_mode;
                            candidate_ptr->angle_delta[PLANE_TYPE_UV] = angle_delta;
                            candidate_ptr->is_directional_chroma_mode_flag =
                                is_directional_chroma_mode_flag;
                            // Update transform_type_uv
                            FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;
                            if (candidate_ptr->intra_chroma_mode == UV_CFL_PRED)
                                candidate_ptr->transform_type_uv = DCT_DCT;
                            else
                                candidate_ptr->transform_type_uv =
#if RFCTR_INTRA_TX_INIT_FUNC
                                    av1_get_tx_type(
                                        0, // is_inter
                                        (PredictionMode)candidate_ptr->intra_luma_mode,
                                        (UvPredictionMode)candidate_ptr->intra_chroma_mode,
                                        PLANE_TYPE_UV,
                                        context_ptr->blk_geom->txsize_uv[0][0],
                                        frm_hdr->reduced_tx_set);
#else
                                    av1_get_tx_type(
                                        context_ptr->blk_geom->bsize,
                                        0,
                                        (PredictionMode)candidate_ptr->intra_luma_mode,
                                        (UvPredictionMode)candidate_ptr->intra_chroma_mode,
                                        PLANE_TYPE_UV,
                                        0,
                                        0,
                                        0,
                                        context_ptr->blk_geom->txsize_uv[0][0],
                                        frm_hdr->reduced_tx_set);
#endif
                        }
                }
            }
        }
    }
}

static void md_stage_3(PictureControlSet *pcs_ptr, SuperBlock *sb_ptr, BlkStruct *blk_ptr,
                       ModeDecisionContext *context_ptr, EbPictureBufferDesc *input_picture_ptr,
                       uint32_t input_origin_index, uint32_t input_cb_origin_in_index,
                       uint32_t blk_origin_index, uint32_t blk_chroma_origin_index,
                       uint32_t fullCandidateTotalCount) {
    ModeDecisionCandidateBuffer **candidate_buffer_ptr_array_base =
        context_ptr->candidate_buffer_ptr_array;
    ModeDecisionCandidateBuffer **candidate_buffer_ptr_array = &(
        candidate_buffer_ptr_array_base[0]);

    for (uint32_t full_loop_candidate_index = 0;
         full_loop_candidate_index < fullCandidateTotalCount;
         ++full_loop_candidate_index) {
        uint32_t cand_index = context_ptr->best_candidate_index_array[full_loop_candidate_index];
#if OPT_LOSSLESS_1
        if (context_ptr->use_best_mds0)
            cand_index = context_ptr->mds0_best_idx;
#endif
        ModeDecisionCandidateBuffer *candidate_buffer = candidate_buffer_ptr_array[cand_index];
        ModeDecisionCandidate *      candidate_ptr    = candidate_buffer->candidate_ptr;
        uint32_t                     reduce_prec      = context_ptr->use_prev_mds_res &&
            (!context_ptr->bypass_md_stage_1[candidate_ptr->cand_class] ||
             !context_ptr->bypass_md_stage_2[candidate_ptr->cand_class]) &&
            (!candidate_buffer->candidate_ptr->block_has_coeff);
#if FTR_REDUCE_MDS3_COMPLEXITY
        // Adjust md stage_count
        uint32_t disable_feature = 0;
        if (context_ptr->reduce_last_md_stage_candidate > 1) {
            if (pcs_ptr->slice_type != I_SLICE) {
                if (context_ptr->mds0_best_idx == context_ptr->mds1_best_idx) {
                    if (context_ptr->mds0_best_idx != cand_index) {
                        //if (candidate_ptr->cand_class != context_ptr->mds1_best_class_it) {
                        disable_feature = 1;
                    }
                }
            }
        }
#endif
        // Set MD Staging full_loop_core settings
        context_ptr->md_staging_perform_inter_pred = context_ptr->md_staging_mode !=
            MD_STAGING_MODE_0;
#if FTR_REDUCE_MDS3_COMPLEXITY
        context_ptr->md_staging_skip_interpolation_search = reduce_prec || disable_feature
#else
        context_ptr->md_staging_skip_interpolation_search = reduce_prec
#endif
            ? 0
            : ((context_ptr->interpolation_search_level == IFS_MDS3) ? EB_FALSE : EB_TRUE);
        context_ptr->md_staging_skip_chroma_pred = EB_FALSE;
        if (context_ptr->md_staging_tx_size_level)
            context_ptr->md_staging_tx_size_mode = 1;
        else
            context_ptr->md_staging_tx_size_mode = candidate_ptr->cand_class == CAND_CLASS_0 ||
                candidate_ptr->cand_class == CAND_CLASS_3;
        context_ptr->md_staging_txt_level        = reduce_prec ? 0 : context_ptr->txt_ctrls.enabled;
        context_ptr->md_staging_skip_full_chroma = EB_FALSE;
#if FTR_REDUCE_MDS3_COMPLEXITY
        context_ptr->md_staging_skip_rdoq = reduce_prec || disable_feature ? EB_TRUE : EB_FALSE;
#else
        context_ptr->md_staging_skip_rdoq = reduce_prec ? EB_TRUE : EB_FALSE;
#endif

        context_ptr->md_staging_spatial_sse_full_loop_level =
            context_ptr->spatial_sse_full_loop_level;

        context_ptr->md_staging_perform_intra_chroma_pred = EB_TRUE;
        if (context_ptr->chroma_at_last_md_stage)
            update_intra_chroma_mode(context_ptr, candidate_ptr, pcs_ptr);
        full_loop_core(pcs_ptr,
                       sb_ptr,
                       blk_ptr,
                       context_ptr,
                       candidate_buffer,
                       candidate_ptr,
                       input_picture_ptr,
                       input_origin_index,
                       input_cb_origin_in_index,
                       blk_origin_index,
                       blk_chroma_origin_index);
    }
}

void move_blk_data(PictureControlSet *pcs, EncDecContext *context_ptr, BlkStruct *src_cu,
                   BlkStruct *dst_cu) {
    svt_memcpy(&dst_cu->palette_info.pmi, &src_cu->palette_info.pmi, sizeof(PaletteModeInfo));
    if (svt_av1_allow_palette(pcs->parent_pcs_ptr->palette_level, context_ptr->blk_geom->bsize)) {
        dst_cu->palette_info.color_idx_map = (uint8_t *)malloc(MAX_PALETTE_SQUARE);
        assert(dst_cu->palette_info.color_idx_map != NULL && "palette:Not-Enough-Memory");
        if (dst_cu->palette_info.color_idx_map != NULL)
            svt_memcpy(dst_cu->palette_info.color_idx_map,
                       src_cu->palette_info.color_idx_map,
                       MAX_PALETTE_SQUARE);
        else
            SVT_LOG("ERROR palette:Not-Enough-Memory\n");
    }
    dst_cu->interp_filters              = src_cu->interp_filters;
    dst_cu->interinter_comp.type        = src_cu->interinter_comp.type;
    dst_cu->interinter_comp.mask_type   = src_cu->interinter_comp.mask_type;
    dst_cu->interinter_comp.wedge_index = src_cu->interinter_comp.wedge_index;
    dst_cu->interinter_comp.wedge_sign  = src_cu->interinter_comp.wedge_sign;
    dst_cu->compound_idx                = src_cu->compound_idx;
    dst_cu->comp_group_idx              = src_cu->comp_group_idx;

    dst_cu->is_interintra_used     = src_cu->is_interintra_used;
    dst_cu->interintra_mode        = src_cu->interintra_mode;
    dst_cu->use_wedge_interintra   = src_cu->use_wedge_interintra;
    dst_cu->interintra_wedge_index = src_cu->interintra_wedge_index; //inter_intra wedge index
    //CHKN TransformUnit             txb_array[TRANSFORM_UNIT_MAX_COUNT]; // 2-bytes * 21 = 42-bytes
    svt_memcpy(
        dst_cu->txb_array, src_cu->txb_array, TRANSFORM_UNIT_MAX_COUNT * sizeof(TransformUnit));

    //CHKN PredictionUnit            prediction_unit_array[MAX_NUM_OF_PU_PER_CU];    // 35-bytes * 4 = 140 bytes
    svt_memcpy(dst_cu->prediction_unit_array,
               src_cu->prediction_unit_array,
               MAX_NUM_OF_PU_PER_CU * sizeof(PredictionUnit));

    dst_cu->skip_flag_context    = src_cu->skip_flag_context;
    dst_cu->prediction_mode_flag = src_cu->prediction_mode_flag;
    dst_cu->block_has_coeff      = src_cu->block_has_coeff;
    dst_cu->split_flag_context   = src_cu->split_flag_context;
    dst_cu->qindex               = src_cu->qindex;
    dst_cu->tx_depth             = src_cu->tx_depth;
    dst_cu->split_flag           = src_cu->split_flag;
    dst_cu->skip_flag            = src_cu->skip_flag;

    //CHKN    MacroBlockD*  av1xd;
#if OPT_INIT_XD_2
    // Don't copy if dest. is NULL
    if (dst_cu->av1xd != NULL)
#endif
    svt_memcpy(dst_cu->av1xd, src_cu->av1xd, sizeof(MacroBlockD));

    // uint8_t ref_mv_count[MODE_CTX_REF_FRAMES];

    //CHKN int16_t inter_mode_ctx[MODE_CTX_REF_FRAMES];
    svt_memcpy(
        dst_cu->inter_mode_ctx, src_cu->inter_mode_ctx, MODE_CTX_REF_FRAMES * sizeof(int16_t));

    //CHKN uint8_t  drl_index;
    //CHKN PredictionMode               pred_mode;
    dst_cu->drl_index = src_cu->drl_index;
    dst_cu->pred_mode = src_cu->pred_mode;

    //CHKN IntMv  predmv[2];

    svt_memcpy(dst_cu->predmv, src_cu->predmv, 2 * sizeof(IntMv));
    //CHKN uint8_t                         skip_coeff_context;
    //CHKN int16_t                        luma_txb_skip_context;
    //CHKN int16_t                        luma_dc_sign_context;
    //CHKN int16_t                        cb_txb_skip_context;
    //CHKN int16_t                        cb_dc_sign_context;
    //CHKN int16_t                        cr_txb_skip_context;
    //CHKN int16_t                        cr_dc_sign_context;
    //CHKN uint8_t                         reference_mode_context;
    //CHKN uint8_t                         compoud_reference_type_context;
    //CHKN uint32_t                        partitionContext;
#if !CLN_MDC_CTX
    dst_cu->reference_mode_context         = src_cu->reference_mode_context;
    dst_cu->compoud_reference_type_context = src_cu->compoud_reference_type_context;
#endif
    dst_cu->segment_id = src_cu->segment_id;

#if !CLN_SB_DATA
    svt_memcpy(dst_cu->quantized_dc, src_cu->quantized_dc, 3 * MAX_TXB_COUNT * sizeof(int32_t));
#endif
    //CHKN uint32_t   is_inter_ctx;
    //CHKN uint32_t                     interp_filters;

    dst_cu->is_inter_ctx   = src_cu->is_inter_ctx;
    dst_cu->interp_filters = src_cu->interp_filters;

    dst_cu->part              = src_cu->part;
    dst_cu->mds_idx           = src_cu->mds_idx;
    dst_cu->filter_intra_mode = src_cu->filter_intra_mode;
    dst_cu->use_intrabc       = src_cu->use_intrabc;
    dst_cu->drl_ctx[0]        = src_cu->drl_ctx[0];
    dst_cu->drl_ctx[1]        = src_cu->drl_ctx[1];
    dst_cu->drl_ctx_near[0]   = src_cu->drl_ctx_near[0];
    dst_cu->drl_ctx_near[1]   = src_cu->drl_ctx_near[1];
}
void move_blk_data_redund(PictureControlSet *pcs, ModeDecisionContext *context_ptr,
                          BlkStruct *src_cu, BlkStruct *dst_cu) {
    dst_cu->segment_id       = src_cu->segment_id;
    dst_cu->seg_id_predicted = src_cu->seg_id_predicted;
    svt_memcpy(&dst_cu->palette_info.pmi, &src_cu->palette_info.pmi, sizeof(PaletteModeInfo));
    if (svt_av1_allow_palette(pcs->parent_pcs_ptr->palette_level, context_ptr->blk_geom->bsize))
        svt_memcpy(dst_cu->palette_info.color_idx_map,
                   src_cu->palette_info.color_idx_map,
                   MAX_PALETTE_SQUARE);
    dst_cu->interp_filters              = src_cu->interp_filters;
    dst_cu->interinter_comp.type        = src_cu->interinter_comp.type;
    dst_cu->interinter_comp.mask_type   = src_cu->interinter_comp.mask_type;
    dst_cu->interinter_comp.wedge_index = src_cu->interinter_comp.wedge_index;
    dst_cu->interinter_comp.wedge_sign  = src_cu->interinter_comp.wedge_sign;
    dst_cu->compound_idx                = src_cu->compound_idx;
    dst_cu->comp_group_idx              = src_cu->comp_group_idx;
    dst_cu->is_interintra_used          = src_cu->is_interintra_used;
    dst_cu->interintra_mode             = src_cu->interintra_mode;
    dst_cu->use_wedge_interintra        = src_cu->use_wedge_interintra;
    dst_cu->interintra_wedge_index      = src_cu->interintra_wedge_index; //inter_intra wedge index
    dst_cu->filter_intra_mode           = src_cu->filter_intra_mode;
    //CHKN TransformUnit_t             txb_array[TRANSFORM_UNIT_MAX_COUNT]; // 2-bytes * 21 = 42-bytes
    svt_memcpy(
        dst_cu->txb_array, src_cu->txb_array, TRANSFORM_UNIT_MAX_COUNT * sizeof(TransformUnit));

    //CHKN PredictionUnit_t            prediction_unit_array[MAX_NUM_OF_PU_PER_CU];    // 35-bytes * 4 = 140 bytes
    svt_memcpy(dst_cu->prediction_unit_array,
               src_cu->prediction_unit_array,
               MAX_NUM_OF_PU_PER_CU * sizeof(PredictionUnit));
    dst_cu->skip_flag_context    = src_cu->skip_flag_context;
    dst_cu->prediction_mode_flag = src_cu->prediction_mode_flag;
    dst_cu->block_has_coeff      = src_cu->block_has_coeff;
    dst_cu->split_flag_context   = src_cu->split_flag_context;
    dst_cu->qindex               = src_cu->qindex;
    dst_cu->skip_flag            = src_cu->skip_flag;
    dst_cu->tx_depth             = src_cu->tx_depth;
    //CHKN    MacroBlockD*  av1xd;
    svt_memcpy(dst_cu->av1xd, src_cu->av1xd, sizeof(MacroBlockD));

    // uint8_t ref_mv_count[MODE_CTX_REF_FRAMES];

    //CHKN int16_t inter_mode_ctx[MODE_CTX_REF_FRAMES];
    svt_memcpy(
        dst_cu->inter_mode_ctx, src_cu->inter_mode_ctx, MODE_CTX_REF_FRAMES * sizeof(int16_t));

    //CHKN uint8_t  drl_index;
    //CHKN PredictionMode               pred_mode;
    dst_cu->drl_index = src_cu->drl_index;
    dst_cu->pred_mode = src_cu->pred_mode;

    //CHKN IntMv  predmv[2];

    svt_memcpy(dst_cu->predmv, src_cu->predmv, 2 * sizeof(IntMv));

    //CHKN uint8_t                         skip_coeff_context;
    //CHKN int16_t                        luma_txb_skip_context;
    //CHKN int16_t                        luma_dc_sign_context;
    //CHKN int16_t                        cb_txb_skip_context;
    //CHKN int16_t                        cb_dc_sign_context;
    //CHKN int16_t                        cr_txb_skip_context;
    //CHKN int16_t                        cr_dc_sign_context;
    //CHKN uint8_t                         reference_mode_context;
    //CHKN uint8_t                         compoud_reference_type_context;
    //CHKN uint32_t                        partitionContext;
#if !CLN_MDC_CTX
    dst_cu->reference_mode_context         = src_cu->reference_mode_context;
    dst_cu->compoud_reference_type_context = src_cu->compoud_reference_type_context;
#endif

#if !CLN_SB_DATA
    svt_memcpy(dst_cu->quantized_dc, src_cu->quantized_dc, 3 * MAX_TXB_COUNT * sizeof(int32_t));
#endif
    //CHKN uint32_t   is_inter_ctx;
    //CHKN uint32_t                     interp_filters;

    dst_cu->is_inter_ctx   = src_cu->is_inter_ctx;
    dst_cu->interp_filters = src_cu->interp_filters;

    dst_cu->part            = src_cu->part;
    dst_cu->use_intrabc     = src_cu->use_intrabc;
    dst_cu->drl_ctx[0]      = src_cu->drl_ctx[0];
    dst_cu->drl_ctx[1]      = src_cu->drl_ctx[1];
    dst_cu->drl_ctx_near[0] = src_cu->drl_ctx_near[0];
    dst_cu->drl_ctx_near[1] = src_cu->drl_ctx_near[1];
    for (int list_idx = 0; list_idx < MAX_NUM_OF_REF_PIC_LIST; list_idx++) {
        for (int ref_idx = 0; ref_idx < MAX_REF_IDX; ref_idx++) {
            context_ptr->sb_me_mv[dst_cu->mds_idx][list_idx][ref_idx][0] =
                context_ptr->sb_me_mv[src_cu->mds_idx][list_idx][ref_idx][0];
            context_ptr->sb_me_mv[dst_cu->mds_idx][list_idx][ref_idx][1] =
                context_ptr->sb_me_mv[src_cu->mds_idx][list_idx][ref_idx][1];
        }
    }
}

void check_redundant_block(const BlockGeom *blk_geom, ModeDecisionContext *context_ptr,
                           uint8_t *redundant_blk_avail, uint16_t *redundant_blk_mds) {
    if (blk_geom->redund) {
        for (int it = 0; it < blk_geom->redund_list.list_size; it++) {
            if (context_ptr->avail_blk_flag[blk_geom->redund_list.blk_mds_table[it]]) {
                *redundant_blk_mds   = blk_geom->redund_list.blk_mds_table[it];
                *redundant_blk_avail = 1;
                break;
            }
        }
    }
}

/*
   search for a valid previously encoded similar
   block (block having the same location and shape as the current block,
   but where neighboring blocks are different from those for the current block)
*/
void check_similar_block(const BlockGeom *blk_geom, ModeDecisionContext *context_ptr,
                         uint8_t *similar_blk_avail, uint16_t *similar_blk_mds) {
    if (blk_geom->similar) {
        for (int it = 0; it < blk_geom->similar_list.list_size; it++) {
            if (context_ptr->avail_blk_flag[blk_geom->similar_list.blk_mds_table[it]]) {
                *similar_blk_mds   = blk_geom->similar_list.blk_mds_table[it];
                *similar_blk_avail = 1;
                break;
            }
        }
    }
}

/******************************************************
* Derive md Settings(feature signals) that could be
  changed  at the block level
******************************************************/
#if OPT_INIT
EbErrorType signal_derivation_block( ModeDecisionContext *context_ptr) {
#else
EbErrorType signal_derivation_block(PictureControlSet *pcs, ModeDecisionContext *context_ptr) {
#endif
    EbErrorType return_error = EB_ErrorNone;
#if !OPT_INIT
    EbEncMode enc_mode = pcs->parent_pcs_ptr->enc_mode;
    // Set dist_based_ref_pruning
#if FTR_NEW_REF_PRUNING_CTRLS
    if (pcs->parent_pcs_ptr->ref_list0_count_try > 1 ||
        pcs->parent_pcs_ptr->ref_list1_count_try > 1) {
        if (context_ptr->pd_pass == PD_PASS_0)
            context_ptr->dist_based_ref_pruning = 0;
        else if (context_ptr->pd_pass == PD_PASS_1)
            context_ptr->dist_based_ref_pruning = 0;
        else if (enc_mode <= ENC_MR)
            context_ptr->dist_based_ref_pruning = 1;
#if TUNE_M0_M3_BASE_NBASE
        else if (enc_mode <= ENC_M0)
            context_ptr->dist_based_ref_pruning = (pcs->temporal_layer_index == 0) ? 1 : 2;
#endif
#if !TUNE_M1_REPOSITION
        else if (enc_mode <= ENC_M1)
            context_ptr->dist_based_ref_pruning = 2;
#endif
#if TUNE_M0_M3_BASE_NBASE
#if TUNE_M3_REPOSITION
#if TUNE_M4_REPOSITION && (!TUNE_SHIFT_PRESETS_DOWN || TUNE_M0_M8_MEGA_FEB)
#if TUNE_M0_M8_MEGA_FEB
        else if (enc_mode <= ENC_M7)
#else
        else if (enc_mode <= ENC_M4)
#endif
#else
        else if (enc_mode <= ENC_M3)
#endif
#else
        else if (enc_mode <= ENC_M2)
#endif
            context_ptr->dist_based_ref_pruning = (pcs->temporal_layer_index == 0) ? 2 : 4;
#endif
#if TUNE_M4_M8
        else
#if TUNE_NEW_PRESETS_MR_M8
            context_ptr->dist_based_ref_pruning = 4;
#else
            context_ptr->dist_based_ref_pruning = 3;
#endif
#else
        else if (enc_mode <= ENC_M5)
            context_ptr->dist_based_ref_pruning = 3;
        else
            context_ptr->dist_based_ref_pruning = 4;
#endif
    } else {
        context_ptr->dist_based_ref_pruning = 0;
    }
#else
    if (pcs->parent_pcs_ptr->ref_list0_count_try > 1 ||
        pcs->parent_pcs_ptr->ref_list1_count_try > 1) {
        if (context_ptr->pd_pass == PD_PASS_0)
            context_ptr->dist_based_ref_pruning = 0;
        else if (context_ptr->pd_pass == PD_PASS_1)
            context_ptr->dist_based_ref_pruning = 0;
        else if (enc_mode <= ENC_MR)
            context_ptr->dist_based_ref_pruning = 1;
        else if (enc_mode <= ENC_M1)
            context_ptr->dist_based_ref_pruning = 3;
        else
            context_ptr->dist_based_ref_pruning = 4;
    } else {
        context_ptr->dist_based_ref_pruning = 0;
    }
#endif
    set_dist_based_ref_pruning_controls(context_ptr, context_ptr->dist_based_ref_pruning);
#endif
    // set compound_types_to_try
    set_inter_comp_controls(context_ptr, context_ptr->inter_compound_mode);
    return return_error;
}

void init_chroma_mode(ModeDecisionContext *context_ptr) {
    EbBool use_angle_delta = av1_use_angle_delta(context_ptr->blk_geom->bsize,
                                                 context_ptr->md_intra_angle_delta);
    for (uint8_t intra_mode = DC_PRED; intra_mode <= PAETH_PRED; ++intra_mode) {
        uint8_t angleDeltaCandidateCount = (use_angle_delta &&
                                            av1_is_directional_mode((PredictionMode)intra_mode))
            ? 7
            : 1;
        uint8_t angle_delta_shift        = 1;
        for (uint8_t angleDeltaCounter = 0; angleDeltaCounter < angleDeltaCandidateCount;
             ++angleDeltaCounter) {
            int32_t angle_delta = CLIP(angle_delta_shift *
                                           (angleDeltaCandidateCount == 1 ? 0
                                                                          : angleDeltaCounter -
                                                    (angleDeltaCandidateCount >> 1)),
                                       -MAX_ANGLE_DELTA,
                                       MAX_ANGLE_DELTA);
            context_ptr->best_uv_mode[intra_mode][MAX_ANGLE_DELTA + angle_delta]  = intra_mode;
            context_ptr->best_uv_angle[intra_mode][MAX_ANGLE_DELTA + angle_delta] = angle_delta;
            context_ptr->best_uv_cost[intra_mode][MAX_ANGLE_DELTA + angle_delta]  = (uint64_t)~0;
        }
    }
}
static void search_best_independent_uv_mode(PictureControlSet *  pcs_ptr,
                                            EbPictureBufferDesc *input_picture_ptr,
                                            uint32_t             input_cb_origin_in_index,
                                            uint32_t             input_cr_origin_in_index,
                                            uint32_t             cu_chroma_origin_index,
                                            ModeDecisionContext *context_ptr) {
    FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;
    uint32_t     full_lambda =
        context_ptr->full_lambda_md[context_ptr->hbd_mode_decision ? EB_10_BIT_MD : EB_8_BIT_MD];
    context_ptr->uv_intra_comp_only = EB_TRUE;

    EbBool use_angle_delta = av1_use_angle_delta(context_ptr->blk_geom->bsize,
                                                 context_ptr->md_intra_angle_delta);

    int coeff_rate[UV_PAETH_PRED + 1][(MAX_ANGLE_DELTA << 1) + 1];
    int distortion[UV_PAETH_PRED + 1][(MAX_ANGLE_DELTA << 1) + 1];

    ModeDecisionCandidate *candidate_array         = context_ptr->fast_candidate_array;
    uint32_t               start_fast_buffer_index = MODE_DECISION_CANDIDATE_MAX_COUNT_Y;

#if CLN_MD_CAND_BUFF
    uint32_t start_full_buffer_index = context_ptr->max_nics;
#else
    uint32_t start_full_buffer_index = MAX_NFL_BUFF_Y;
#endif
    uint32_t uv_mode_total_count = start_fast_buffer_index;
    // Shut RDOQ
    context_ptr->md_staging_skip_rdoq = 0;
    UvPredictionMode uv_mode_end      = context_ptr->md_enable_paeth ? UV_PAETH_PRED
             : context_ptr->md_enable_smooth                         ? UV_SMOOTH_H_PRED
                                                                     : UV_D67_PRED;

    uint8_t uv_mode_start                           = UV_DC_PRED;
    uint8_t disable_angle_prediction                = 0;
    uint8_t directional_mode_skip_mask[INTRA_MODES] = {0};
    for (UvPredictionMode uv_mode = uv_mode_start; uv_mode <= uv_mode_end; ++uv_mode) {
        uint8_t uv_angle_delta_candidate_count = (use_angle_delta &&
                                                  av1_is_directional_mode((PredictionMode)uv_mode))
            ? 7
            : 1;
        if (!av1_is_directional_mode((PredictionMode)uv_mode) ||
            (!disable_angle_prediction &&
             directional_mode_skip_mask[(PredictionMode)uv_mode] == 0)) {
            for (uint8_t uv_angle_delta_counter = 0;
                 uv_angle_delta_counter < uv_angle_delta_candidate_count;
                 ++uv_angle_delta_counter) {
                const uint8_t uv_angle_delta_shift = 1;
                int32_t       uv_angle_delta       = CLIP(
                    uv_angle_delta_shift *
                        (uv_angle_delta_candidate_count == 1
                                         ? 0
                                         : uv_angle_delta_counter - (uv_angle_delta_candidate_count >> 1)),
                    -MAX_ANGLE_DELTA,
                    MAX_ANGLE_DELTA);
                candidate_array[uv_mode_total_count].type = INTRA_MODE;
#if !CLN_MD_CANDS
                candidate_array[uv_mode_total_count].distortion_ready = 0;
#endif
                candidate_array[uv_mode_total_count].use_intrabc                     = 0;
                candidate_array[uv_mode_total_count].angle_delta[PLANE_TYPE_UV]      = 0;
                candidate_array[uv_mode_total_count].pred_mode                       = DC_PRED;
                candidate_array[uv_mode_total_count].intra_chroma_mode               = uv_mode;
                candidate_array[uv_mode_total_count].is_directional_chroma_mode_flag = (uint8_t)
                    av1_is_directional_mode((PredictionMode)uv_mode);
                candidate_array[uv_mode_total_count].angle_delta[PLANE_TYPE_UV] = uv_angle_delta;
                candidate_array[uv_mode_total_count].tx_depth                   = 0;
                candidate_array[uv_mode_total_count].palette_info               = NULL;
                candidate_array[uv_mode_total_count].filter_intra_mode = FILTER_INTRA_MODES;
                candidate_array[uv_mode_total_count].cfl_alpha_signs   = 0;
                candidate_array[uv_mode_total_count].cfl_alpha_idx     = 0;
                candidate_array[uv_mode_total_count].transform_type[0] = DCT_DCT;
                candidate_array[uv_mode_total_count].ref_frame_type    = INTRA_FRAME;
                candidate_array[uv_mode_total_count].motion_mode       = SIMPLE_TRANSLATION;
#if RFCTR_INTRA_TX_INIT_FUNC
                candidate_array[uv_mode_total_count].transform_type_uv = av1_get_tx_type(
                    0, // is_inter
                    (PredictionMode)0,
                    (UvPredictionMode)uv_mode,
                    PLANE_TYPE_UV,
                    context_ptr->blk_geom->txsize_uv[0][0],
                    frm_hdr->reduced_tx_set);
#else
                candidate_array[uv_mode_total_count].transform_type_uv = av1_get_tx_type(
                    context_ptr->blk_geom->bsize,
                    0,
                    (PredictionMode)NULL,
                    (UvPredictionMode)uv_mode,
                    PLANE_TYPE_UV,
                    0,
                    0,
                    0,
                    context_ptr->blk_geom->txsize_uv[0][0],
                    frm_hdr->reduced_tx_set);
#endif
                uv_mode_total_count++;
            }
        }
    }
    uv_mode_total_count = uv_mode_total_count - start_fast_buffer_index;
    // Fast-loop search uv_mode
    for (uint8_t uv_mode_count = 0; uv_mode_count < uv_mode_total_count; uv_mode_count++) {
        ModeDecisionCandidateBuffer *candidate_buffer =
            context_ptr->candidate_buffer_ptr_array[uv_mode_count + start_full_buffer_index];
        candidate_buffer->candidate_ptr =
            &context_ptr->fast_candidate_array[uv_mode_count + start_fast_buffer_index];

        context_ptr->md_staging_skip_chroma_pred = EB_FALSE;
        svt_product_prediction_fun_table[candidate_buffer->candidate_ptr->type](
            context_ptr->hbd_mode_decision, context_ptr, pcs_ptr, candidate_buffer);

        uint32_t chroma_fast_distortion;
        if (!context_ptr->hbd_mode_decision) {
            chroma_fast_distortion = svt_nxm_sad_kernel_sub_sampled(
                input_picture_ptr->buffer_cb + input_cb_origin_in_index,
                input_picture_ptr->stride_cb,
                candidate_buffer->prediction_ptr->buffer_cb + cu_chroma_origin_index,
                candidate_buffer->prediction_ptr->stride_cb,
                context_ptr->blk_geom->bheight_uv,
                context_ptr->blk_geom->bwidth_uv);

            chroma_fast_distortion += svt_nxm_sad_kernel_sub_sampled(
                input_picture_ptr->buffer_cr + input_cr_origin_in_index,
                input_picture_ptr->stride_cr,
                candidate_buffer->prediction_ptr->buffer_cr + cu_chroma_origin_index,
                candidate_buffer->prediction_ptr->stride_cr,
                context_ptr->blk_geom->bheight_uv,
                context_ptr->blk_geom->bwidth_uv);
        } else {
            chroma_fast_distortion = sad_16b_kernel(
                ((uint16_t *)input_picture_ptr->buffer_cb) + input_cb_origin_in_index,
                input_picture_ptr->stride_cb,
                ((uint16_t *)candidate_buffer->prediction_ptr->buffer_cb) + cu_chroma_origin_index,
                candidate_buffer->prediction_ptr->stride_cb,
                context_ptr->blk_geom->bheight_uv,
                context_ptr->blk_geom->bwidth_uv);

            chroma_fast_distortion += sad_16b_kernel(
                ((uint16_t *)input_picture_ptr->buffer_cr) + input_cr_origin_in_index,
                input_picture_ptr->stride_cr,
                ((uint16_t *)candidate_buffer->prediction_ptr->buffer_cr) + cu_chroma_origin_index,
                candidate_buffer->prediction_ptr->stride_cr,
                context_ptr->blk_geom->bheight_uv,
                context_ptr->blk_geom->bwidth_uv);
        }
        // Do not consider rate @ this stage
        *(candidate_buffer->fast_cost_ptr) = chroma_fast_distortion;
    }

    // Sort uv_mode (in terms of distortion only)
#if CLN_MD_CAND_BUFF
    uint32_t *uv_cand_buff_indices = (uint32_t *)malloc(context_ptr->max_nics_uv *
                                                        sizeof(*uv_cand_buff_indices));
    memset(uv_cand_buff_indices, 0xFF, context_ptr->max_nics_uv * sizeof(*uv_cand_buff_indices));

#else
    uint32_t uv_cand_buff_indices[MAX_NFL_BUFF_Y];
    memset(uv_cand_buff_indices, 0xFF, MAX_NFL_BUFF_Y * sizeof(*uv_cand_buff_indices));
#endif
    sort_fast_cost_based_candidates(
        context_ptr,
        start_full_buffer_index,
        uv_mode_total_count, //how many cand buffers to sort. one of the buffers can have max cost.
        uv_cand_buff_indices);

    // Reset *(candidate_buffer->fast_cost_ptr)
    for (uint8_t uv_mode_count = 0; uv_mode_count < uv_mode_total_count; uv_mode_count++) {
        ModeDecisionCandidateBuffer *candidate_buffer =
            context_ptr->candidate_buffer_ptr_array[uv_mode_count + start_full_buffer_index];
        *(candidate_buffer->fast_cost_ptr) = MAX_CU_COST;
    }

    // Derive uv_mode_nfl_count
    uint8_t uv_mode_nfl_count = !pcs_ptr->temporal_layer_index ? uv_mode_total_count
        : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag   ? 16
                                                               : 8;

    // Full-loop search uv_mode
    for (uint8_t uv_mode_count = 0; uv_mode_count < MIN(uv_mode_total_count, uv_mode_nfl_count);
         uv_mode_count++) {
        ModeDecisionCandidateBuffer *candidate_buffer =
            context_ptr->candidate_buffer_ptr_array[uv_cand_buff_indices[uv_mode_count]];
        candidate_buffer->candidate_ptr =
            &context_ptr->fast_candidate_array[uv_cand_buff_indices[uv_mode_count] -
                                               start_full_buffer_index + start_fast_buffer_index];
        uint16_t cb_qindex                           = context_ptr->qp_index;
        uint16_t cr_qindex                           = cb_qindex;
        uint64_t cb_coeff_bits                       = 0;
        uint64_t cr_coeff_bits                       = 0;
        uint64_t cb_full_distortion[DIST_CALC_TOTAL] = {0, 0};
        uint64_t cr_full_distortion[DIST_CALC_TOTAL] = {0, 0};

        uint32_t count_non_zero_coeffs[3][MAX_NUM_OF_TU_PER_CU];

        //Cb Residual
        residual_kernel(input_picture_ptr->buffer_cb,
                        input_cb_origin_in_index,
                        input_picture_ptr->stride_cb,
                        candidate_buffer->prediction_ptr->buffer_cb,
                        cu_chroma_origin_index,
                        candidate_buffer->prediction_ptr->stride_cb,
                        (int16_t *)candidate_buffer->residual_ptr->buffer_cb,
                        cu_chroma_origin_index,
                        candidate_buffer->residual_ptr->stride_cb,
                        context_ptr->hbd_mode_decision,
                        context_ptr->blk_geom->bwidth_uv,
                        context_ptr->blk_geom->bheight_uv);

        //Cr Residual
        residual_kernel(input_picture_ptr->buffer_cr,
                        input_cr_origin_in_index,
                        input_picture_ptr->stride_cr,
                        candidate_buffer->prediction_ptr->buffer_cr,
                        cu_chroma_origin_index,
                        candidate_buffer->prediction_ptr->stride_cr,
                        (int16_t *)candidate_buffer->residual_ptr->buffer_cr,
                        cu_chroma_origin_index,
                        candidate_buffer->residual_ptr->stride_cr,
                        context_ptr->hbd_mode_decision,
                        context_ptr->blk_geom->bwidth_uv,
                        context_ptr->blk_geom->bheight_uv);

        full_loop_r(context_ptr->sb_ptr,
                    candidate_buffer,
                    context_ptr,
                    input_picture_ptr,
                    pcs_ptr,
                    PICTURE_BUFFER_DESC_CHROMA_MASK,
                    cb_qindex,
                    cr_qindex,
                    &(*count_non_zero_coeffs[1]),
                    &(*count_non_zero_coeffs[2]));

        cu_full_distortion_fast_txb_mode_r(context_ptr->sb_ptr,
                                           candidate_buffer,
                                           context_ptr,
                                           candidate_buffer->candidate_ptr,
                                           pcs_ptr,
                                           input_picture_ptr,
                                           cb_full_distortion,
                                           cr_full_distortion,
                                           count_non_zero_coeffs,
                                           COMPONENT_CHROMA,
                                           &cb_coeff_bits,
                                           &cr_coeff_bits,
                                           1);

        coeff_rate[candidate_buffer->candidate_ptr->intra_chroma_mode]
                  [MAX_ANGLE_DELTA + candidate_buffer->candidate_ptr->angle_delta[PLANE_TYPE_UV]] =
                      (int)(cb_coeff_bits + cr_coeff_bits);
        distortion[candidate_buffer->candidate_ptr->intra_chroma_mode]
                  [MAX_ANGLE_DELTA + candidate_buffer->candidate_ptr->angle_delta[PLANE_TYPE_UV]] =
                      (int)(cb_full_distortion[DIST_CALC_RESIDUAL] +
                            cr_full_distortion[DIST_CALC_RESIDUAL]);
    }

    // Loop over all intra mode, then over all uv move to derive the best uv mode for a given intra mode in term of rate

    uint8_t intra_mode_end = context_ptr->md_enable_paeth ? PAETH_PRED
        : context_ptr->md_enable_smooth                   ? SMOOTH_H_PRED
                                                          : D67_PRED;
    // intra_mode loop (luma mode loop)
    for (uint8_t intra_mode = DC_PRED; intra_mode <= intra_mode_end; ++intra_mode) {
        uint8_t angle_delta_candidate_count = (use_angle_delta &&
                                               av1_is_directional_mode((PredictionMode)intra_mode))
            ? 7
            : 1;
        uint8_t angle_delta_shift           = 1;

        for (uint8_t angle_delta_counter = 0; angle_delta_counter < angle_delta_candidate_count;
             ++angle_delta_counter) {
            int32_t angle_delta = CLIP(angle_delta_shift *
                                           (angle_delta_candidate_count == 1 ? 0
                                                                             : angle_delta_counter -
                                                    (angle_delta_candidate_count >> 1)),
                                       -MAX_ANGLE_DELTA,
                                       MAX_ANGLE_DELTA);

            // uv mode loop
            context_ptr->best_uv_cost[intra_mode][MAX_ANGLE_DELTA + angle_delta] = (uint64_t)~0;

            for (uint8_t uv_mode_count = 0;
                 uv_mode_count < MIN(uv_mode_total_count, uv_mode_nfl_count);
                 uv_mode_count++) {
                ModeDecisionCandidate *candidate_ptr = &(
                    context_ptr
                        ->fast_candidate_array[uv_cand_buff_indices[uv_mode_count] -
                                               start_full_buffer_index + start_fast_buffer_index]);

                candidate_ptr->intra_luma_mode          = intra_mode;
                candidate_ptr->is_directional_mode_flag = (uint8_t)av1_is_directional_mode(
                    (PredictionMode)intra_mode);
                candidate_ptr->angle_delta[PLANE_TYPE_Y] = angle_delta;
                candidate_ptr->pred_mode                 = (PredictionMode)intra_mode;

                // Fast Cost
                av1_product_fast_cost_func_table[candidate_ptr->type](
#if CLN_FAST_COST
                    context_ptr,
#endif
                    context_ptr->blk_ptr,
                    candidate_ptr,
                    NOT_USED_VALUE,
                    0,
                    0,
                    0,
                    pcs_ptr,
                    &(context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                          .ed_ref_mv_stack[candidate_ptr->ref_frame_type][0]),
                    context_ptr->blk_geom,
                    context_ptr->blk_origin_y >> MI_SIZE_LOG2,
                    context_ptr->blk_origin_x >> MI_SIZE_LOG2,
                    context_ptr->inter_intra_comp_ctrls.enabled,
#if !CLN_FAST_COST
                    1,
#endif
                    context_ptr->intra_luma_left_mode,
                    context_ptr->intra_luma_top_mode);

                uint64_t rate =
                    coeff_rate[candidate_ptr->intra_chroma_mode]
                              [MAX_ANGLE_DELTA + candidate_ptr->angle_delta[PLANE_TYPE_UV]] +
                    candidate_ptr->fast_luma_rate + candidate_ptr->fast_chroma_rate;
                uint64_t uv_cost = RDCOST(
                    full_lambda,
                    rate,
                    distortion[candidate_ptr->intra_chroma_mode]
                              [MAX_ANGLE_DELTA + candidate_ptr->angle_delta[PLANE_TYPE_UV]]);

                if (uv_cost <
                    context_ptr->best_uv_cost[intra_mode][MAX_ANGLE_DELTA + angle_delta]) {
                    context_ptr->best_uv_mode[intra_mode][MAX_ANGLE_DELTA + angle_delta] =
                        candidate_ptr->intra_chroma_mode;
                    context_ptr->best_uv_angle[intra_mode][MAX_ANGLE_DELTA + angle_delta] =
                        candidate_ptr->angle_delta[PLANE_TYPE_UV];

                    context_ptr->best_uv_cost[intra_mode][MAX_ANGLE_DELTA + angle_delta] = uv_cost;
                    context_ptr->fast_luma_rate[intra_mode][MAX_ANGLE_DELTA + angle_delta] =
                        candidate_ptr->fast_luma_rate;
                    context_ptr->fast_chroma_rate[intra_mode][MAX_ANGLE_DELTA + angle_delta] =
                        candidate_ptr->fast_chroma_rate;
                }
            }
        }
    }

#if CLN_MD_CAND_BUFF
    free(uv_cand_buff_indices);
#endif
}
void interintra_class_pruning_1(ModeDecisionContext *context_ptr, uint64_t best_md_stage_cost,
                                uint8_t best_md_stage_pred_mode) {
    for (CandClass cand_class_it = CAND_CLASS_0; cand_class_it < CAND_CLASS_TOTAL;
         cand_class_it++) {
#if FTR_NIC_PRUNING // 1st check
#if TUNE_NEW_PRESETS_MR_M8
        uint64_t mds1_cand_th = context_ptr->nic_pruning_ctrls.mds1_cand_base_th;
#else
        uint64_t mds1_cand_th =
            ((context_ptr->nic_pruning_ctrls.mds1_cand_base_th == (uint64_t)~0) ||
             (context_ptr->nic_pruning_ctrls.mds1_cand_intra_class_offset_th == (uint64_t)~0 &&
              (cand_class_it == CAND_CLASS_0 || cand_class_it == CAND_CLASS_3)) ||
             (context_ptr->nic_pruning_ctrls.mds1_cand_sq_offset_th == (uint64_t)~0 &&
              context_ptr->blk_geom->shape == PART_N))
            ? (uint64_t)~0
            : context_ptr->nic_pruning_ctrls.mds1_cand_base_th +
                ((cand_class_it == CAND_CLASS_0 || cand_class_it == CAND_CLASS_3)
                     ? context_ptr->nic_pruning_ctrls.mds1_cand_intra_class_offset_th
                     : 0) +
                ((context_ptr->blk_geom->shape == PART_N)
                     ? context_ptr->nic_pruning_ctrls.mds1_cand_sq_offset_th
                     : 0);
#endif

        if (mds1_cand_th != (uint64_t)~0 ||
            context_ptr->nic_pruning_ctrls.mds1_class_th != (uint64_t)~0)
#else
        if (context_ptr->md_stage_1_cand_prune_th != (uint64_t)~0 ||
            context_ptr->md_stage_1_class_prune_th != (uint64_t)~0)
#endif
            if (context_ptr->md_stage_0_count[cand_class_it] > 0 &&
                context_ptr->md_stage_1_count[cand_class_it] > 0) {
                uint32_t *cand_buff_indices = context_ptr->cand_buff_indices[cand_class_it];
                uint64_t  class_best_cost   = *(
                    context_ptr->candidate_buffer_ptr_array[cand_buff_indices[0]]->fast_cost_ptr);
                if (context_ptr->early_cand_elimination)
                    if (((best_md_stage_pred_mode == NEAREST_NEARESTMV ||
                          best_md_stage_pred_mode == NEAR_NEARMV)) &&
                        ((cand_class_it == CAND_CLASS_0) || (cand_class_it == CAND_CLASS_3)))
                        context_ptr->md_stage_1_count[cand_class_it] = 0;

                        // inter class pruning
#if FTR_NIC_PRUNING //1st check
                if (class_best_cost && best_md_stage_cost) {
                    uint64_t dev = ((class_best_cost - best_md_stage_cost) * 100) /
                        best_md_stage_cost;
                    if (dev) {
                        if (dev >= context_ptr->nic_pruning_ctrls.mds1_class_th) {
                            context_ptr->md_stage_1_count[cand_class_it] = 0;
                            continue;
                        } else if (context_ptr->nic_pruning_ctrls.mds1_band_cnt >= 3 &&
                                   context_ptr->md_stage_1_count[cand_class_it] > 1) {
                            uint8_t band_idx = (uint8_t)(
                                dev * (context_ptr->nic_pruning_ctrls.mds1_band_cnt - 1) /
                                context_ptr->nic_pruning_ctrls.mds1_class_th);
#if TUNE_LOWER_PRESETS
#if TUNE_NEW_PRESETS_MR_M8
                            context_ptr->md_stage_1_count[cand_class_it] = DIVIDE_AND_ROUND(
                                context_ptr->md_stage_1_count[cand_class_it], band_idx + 1);
#else
                            context_ptr->md_stage_1_count[cand_class_it] = DIVIDE_AND_ROUND(
                                context_ptr->md_stage_1_count[cand_class_it],
                                context_ptr->nic_pruning_ctrls.mds1_scaling_factor *
                                    (band_idx + 1));
#endif
#else
                            context_ptr->md_stage_1_count[cand_class_it] = MAX(
                                1,
                                DIVIDE_AND_ROUND(
                                    context_ptr->md_stage_1_count[cand_class_it],
                                    context_ptr->nic_pruning_ctrls.mds1_scaling_factor *
                                        (band_idx + 1)));
#endif
                        }
                    }
                }
#else
                if (best_md_stage_cost && class_best_cost &&
                    ((((class_best_cost - best_md_stage_cost) * 100) / best_md_stage_cost) >
#if FTR_NIC_PRUNING
                     context_ptr->nic_pruning_ctrls.mds1_class_th)) {
#else
                     context_ptr->md_stage_1_class_prune_th)) {
#endif
                    context_ptr->md_stage_1_count[cand_class_it] = 0;
                    continue;
                }
#endif
                // intra class pruning
                uint32_t cand_count = 1;
                if (class_best_cost)
                    while (
                        cand_count < context_ptr->md_stage_1_count[cand_class_it] &&
                        ((((*(context_ptr->candidate_buffer_ptr_array[cand_buff_indices[cand_count]]
                                  ->fast_cost_ptr) -
                            class_best_cost) *
                           100) /
#if FTR_NIC_PRUNING
                          class_best_cost) < mds1_cand_th)) {
#else
                          class_best_cost) < context_ptr->md_stage_1_cand_prune_th)) {
#endif
                        cand_count++;
                    }
                context_ptr->md_stage_1_count[cand_class_it] = cand_count;
            }
        context_ptr->md_stage_1_total_count += context_ptr->md_stage_1_count[cand_class_it];
    }
}
#if !FTR_NIC_PRUNING
uint32_t class_prune_scale_factor[3 /*levels*/][4 /*band*/][2 /*num/denum*/] = {
    // level0 -- class prune OFF
    {
        {1, 1}, // b0
        {1, 1}, // b1
        {1, 1}, // b2
        {1, 1} // b3
    },
    // level1
    {
        {4, 8}, // b0
        {3, 8}, // b1
        {2, 8}, // b2
        {0, 8} // b3
    },
    // level2
    {
        {2, 8}, // b0
        {1, 8}, // b1
        {1, 8}, // b2
        {0, 8} // b3
    }};

static void class_pruning(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                          uint64_t best_md_stage_cost, uint64_t class_best_cost,
                          CandClass cand_class_it) {
    // band THs
    const uint64_t distance_cost = (class_best_cost - best_md_stage_cost) * 100,
                   band1_cost_th = (class_best_cost * 5), band2_cost_th = (class_best_cost * 10),
                   band3_cost_th = (class_best_cost * 20);

    const unsigned class_pruning_scaling_level = pcs_ptr->enc_mode <= ENC_MRS
        // class prune OFF
        ? 0
        : pcs_ptr->enc_mode <= ENC_M9 ? 1 : 2;
    // minimum nics
    const uint32_t min_nics = pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag
        ? MIN(2, context_ptr->md_stage_2_count[cand_class_it])
        : 1;

    // get the band
    const uint8_t band = distance_cost < band1_cost_th ? 0
        : distance_cost < band2_cost_th                ? 1
        : distance_cost < band3_cost_th                ? 2
                                                       : 3;
    if (class_pruning_scaling_level && best_md_stage_cost && class_best_cost &&
        class_best_cost > best_md_stage_cost) {
        // scale NICS of the worst classes
        const uint32_t scale_num = class_prune_scale_factor[class_pruning_scaling_level][band][0];
        if (scale_num == 0)
            context_ptr->md_stage_2_count[cand_class_it] = 0;
        else {
            const uint32_t scale_denum =
                class_prune_scale_factor[class_pruning_scaling_level][band][1];
            context_ptr->md_stage_2_count[cand_class_it] = MAX(
                min_nics,
                DIVIDE_AND_ROUND(context_ptr->md_stage_2_count[cand_class_it] * scale_num,
                                 scale_denum));
        }
    }
}
#endif
void interintra_class_pruning_2(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                                uint64_t best_md_stage_cost) {
    for (CandClass cand_class_it = CAND_CLASS_0; cand_class_it < CAND_CLASS_TOTAL;
         cand_class_it++) {
#if FTR_REDUCE_MDS2_CAND
        // Adjust md stage_count
#if FTR_REDUCE_MDS3_COMPLEXITY
        if (context_ptr->reduce_last_md_stage_candidate > 2) {
#else
        if (context_ptr->reduce_last_md_stage_candidate) {
#endif
            if (pcs_ptr->slice_type != I_SLICE) {
                if (context_ptr->mds0_best_idx == context_ptr->mds1_best_idx) {
                    if (cand_class_it != context_ptr->mds1_best_class_it) {
                        context_ptr->md_stage_3_count[cand_class_it] = 0;
                        context_ptr->md_stage_2_count[cand_class_it] = 0;
                    }
                }
            }
        }
#endif
#if FTR_NIC_PRUNING // 1st check
#if TUNE_NEW_PRESETS_MR_M8
        uint64_t mds2_cand_th = context_ptr->nic_pruning_ctrls.mds2_cand_base_th;
#else
        uint64_t mds2_cand_th =
            ((context_ptr->nic_pruning_ctrls.mds2_cand_base_th == (uint64_t)~0) ||
             (context_ptr->nic_pruning_ctrls.mds2_cand_intra_class_offset_th == (uint64_t)~0 &&
              (cand_class_it == CAND_CLASS_0 || cand_class_it == CAND_CLASS_3)) ||
             (context_ptr->nic_pruning_ctrls.mds2_cand_sq_offset_th == (uint64_t)~0 &&
              context_ptr->blk_geom->shape == PART_N))
            ? (uint64_t)~0
            : context_ptr->nic_pruning_ctrls.mds2_cand_base_th +
                ((cand_class_it == CAND_CLASS_0 || cand_class_it == CAND_CLASS_3)
                     ? context_ptr->nic_pruning_ctrls.mds2_cand_intra_class_offset_th
                     : 0) +
                ((context_ptr->blk_geom->shape == PART_N)
                     ? context_ptr->nic_pruning_ctrls.mds2_cand_sq_offset_th
                     : 0);
#endif

        if (mds2_cand_th != (uint64_t)~0 ||
            context_ptr->nic_pruning_ctrls.mds2_class_th != (uint64_t)~0)
#else
        if (context_ptr->md_stage_2_cand_prune_th != (uint64_t)~0 ||
            context_ptr->md_stage_2_class_prune_th != (uint64_t)~0)
#endif
            if (context_ptr->md_stage_1_count[cand_class_it] > 0 &&
                context_ptr->md_stage_2_count[cand_class_it] > 0 &&
                context_ptr->bypass_md_stage_1[cand_class_it] == EB_FALSE) {
                uint32_t *cand_buff_indices = context_ptr->cand_buff_indices[cand_class_it];
                uint64_t  class_best_cost   = *(
                    context_ptr->candidate_buffer_ptr_array[cand_buff_indices[0]]->full_cost_ptr);

                // class pruning
#if FTR_NIC_PRUNING
                if (class_best_cost && best_md_stage_cost) {
                    uint64_t dev = ((class_best_cost - best_md_stage_cost) * 100) /
                        best_md_stage_cost;
                    if (dev) {
                        if (dev >= context_ptr->nic_pruning_ctrls.mds2_class_th) {
                            context_ptr->md_stage_2_count[cand_class_it] = 0;
                            continue;
                        } else if (context_ptr->nic_pruning_ctrls.mds2_band_cnt >= 3 &&
                                   context_ptr->md_stage_2_count[cand_class_it] > 1) {
                            uint8_t band_idx = (uint8_t)(
                                dev * (context_ptr->nic_pruning_ctrls.mds2_band_cnt - 1) /
                                context_ptr->nic_pruning_ctrls.mds2_class_th);
#if TUNE_LOWER_PRESETS
#if TUNE_NEW_PRESETS_MR_M8
                            context_ptr->md_stage_2_count[cand_class_it] = DIVIDE_AND_ROUND(
                                context_ptr->md_stage_2_count[cand_class_it], band_idx + 1);
#else
                            context_ptr->md_stage_2_count[cand_class_it] = DIVIDE_AND_ROUND(
                                context_ptr->md_stage_2_count[cand_class_it],
                                context_ptr->nic_pruning_ctrls.mds2_scaling_factor *
                                    (band_idx + 1));
#endif
#else
                            context_ptr->md_stage_2_count[cand_class_it] = MAX(
                                1,
                                DIVIDE_AND_ROUND(
                                    context_ptr->md_stage_2_count[cand_class_it],
                                    context_ptr->nic_pruning_ctrls.mds2_scaling_factor *
                                        (band_idx + 1)));
#endif
                        }
                    }
                }
#else
                class_pruning(
                    pcs_ptr, context_ptr, best_md_stage_cost, class_best_cost, cand_class_it);
#endif
                // intra class pruning
#if !FTR_NIC_PRUNING
                uint64_t md_stage_2_cand_prune_th = context_ptr->md_stage_2_cand_prune_th;
                md_stage_2_cand_prune_th          = (cand_class_it == CAND_CLASS_0 ||
                                            cand_class_it == CAND_CLASS_3)
                             ? (uint64_t)~0
                             : (context_ptr->blk_geom->shape == PART_N) ? (uint64_t)~0
                                                                        : md_stage_2_cand_prune_th;
#endif
                // candidate pruning
                if (context_ptr->md_stage_2_count[cand_class_it] > 0) {
                    uint32_t cand_count = 1;

                    if (class_best_cost)
                        while (cand_count < context_ptr->md_stage_2_count[cand_class_it] &&
                               ((((*(context_ptr
                                         ->candidate_buffer_ptr_array[cand_buff_indices[cand_count]]
                                         ->full_cost_ptr) -
                                   class_best_cost) *
                                  100) /
#if FTR_NIC_PRUNING
                                 class_best_cost) < mds2_cand_th)) {
#else
                                 class_best_cost) < md_stage_2_cand_prune_th)) {
#endif
                            cand_count++;
                        }
                    context_ptr->md_stage_2_count[cand_class_it] = cand_count;
                }
            }
        context_ptr->md_stage_2_total_count += context_ptr->md_stage_2_count[cand_class_it];
    }
}

void interintra_class_pruning_3(ModeDecisionContext *context_ptr, uint64_t best_md_stage_cost) {
    context_ptr->md_stage_3_total_count = 0;
    for (CandClass cand_class_it = CAND_CLASS_0; cand_class_it < CAND_CLASS_TOTAL;
         cand_class_it++) {
#if FTR_NIC_PRUNING // 1st check
#if TUNE_NEW_PRESETS_MR_M8
        uint64_t mds3_cand_th = context_ptr->nic_pruning_ctrls.mds3_cand_base_th;
#else
        uint64_t mds3_cand_th =
            ((context_ptr->nic_pruning_ctrls.mds3_cand_base_th == (uint64_t)~0) ||
             (context_ptr->nic_pruning_ctrls.mds3_cand_intra_class_offset_th == (uint64_t)~0 &&
              (cand_class_it == CAND_CLASS_0 || cand_class_it == CAND_CLASS_3)) ||
             (context_ptr->nic_pruning_ctrls.mds3_cand_sq_offset_th == (uint64_t)~0 &&
              context_ptr->blk_geom->shape == PART_N))
            ? (uint64_t)~0
            : context_ptr->nic_pruning_ctrls.mds3_cand_base_th +
                ((cand_class_it == CAND_CLASS_0 || cand_class_it == CAND_CLASS_3)
                     ? context_ptr->nic_pruning_ctrls.mds3_cand_intra_class_offset_th
                     : 0) +
                ((context_ptr->blk_geom->shape == PART_N)
                     ? context_ptr->nic_pruning_ctrls.mds3_cand_sq_offset_th
                     : 0);
#endif

        if (mds3_cand_th != (uint64_t)~0 ||
            context_ptr->nic_pruning_ctrls.mds3_class_th != (uint64_t)~0)
#else
        if (context_ptr->md_stage_3_cand_prune_th != (uint64_t)~0 ||
            context_ptr->md_stage_3_class_prune_th != (uint64_t)~0)
#endif
            if (context_ptr->md_stage_2_count[cand_class_it] > 0 &&
                context_ptr->md_stage_3_count[cand_class_it] > 0 &&
                context_ptr->bypass_md_stage_2[cand_class_it] == EB_FALSE) {
                uint32_t *cand_buff_indices = context_ptr->cand_buff_indices[cand_class_it];
                uint64_t  class_best_cost   = *(
                    context_ptr->candidate_buffer_ptr_array[cand_buff_indices[0]]->full_cost_ptr);

                // inter class pruning
#if FTR_NIC_PRUNING
                if (class_best_cost && best_md_stage_cost) {
                    uint64_t dev = ((class_best_cost - best_md_stage_cost) * 100) /
                        best_md_stage_cost;
                    if (dev) {
                        if (dev >= context_ptr->nic_pruning_ctrls.mds3_class_th) {
                            context_ptr->md_stage_3_count[cand_class_it] = 0;
                            continue;
                        } else if (context_ptr->nic_pruning_ctrls.mds3_band_cnt >= 3 &&
                                   context_ptr->md_stage_3_count[cand_class_it] > 1) {
                            uint8_t band_idx = (uint8_t)(
                                dev * (context_ptr->nic_pruning_ctrls.mds3_band_cnt - 1) /
                                context_ptr->nic_pruning_ctrls.mds3_class_th);
#if TUNE_LOWER_PRESETS
#if TUNE_NEW_PRESETS_MR_M8
                            context_ptr->md_stage_3_count[cand_class_it] = DIVIDE_AND_ROUND(
                                context_ptr->md_stage_3_count[cand_class_it], band_idx + 1);
#else
                            context_ptr->md_stage_3_count[cand_class_it] = DIVIDE_AND_ROUND(
                                context_ptr->md_stage_3_count[cand_class_it],
                                context_ptr->nic_pruning_ctrls.mds3_scaling_factor *
                                    (band_idx + 1));
#endif
#else
                            context_ptr->md_stage_3_count[cand_class_it] = MAX(
                                1,
                                DIVIDE_AND_ROUND(
                                    context_ptr->md_stage_3_count[cand_class_it],
                                    context_ptr->nic_pruning_ctrls.mds3_scaling_factor *
                                        (band_idx + 1)));
#endif
                        }
                    }
                }
#else
                if (best_md_stage_cost && class_best_cost &&
                    ((((class_best_cost - best_md_stage_cost) * 100) / best_md_stage_cost) >
#if FTR_NIC_PRUNING
                     context_ptr->nic_pruning_ctrls.mds3_class_th)) {
#else
                     context_ptr->md_stage_3_class_prune_th)) {
#endif
                    context_ptr->md_stage_3_count[cand_class_it] = 0;
                    continue;
                }
#endif
                // intra class pruning
                uint32_t cand_count = 1;
                if (class_best_cost)
                    while (
                        cand_count < context_ptr->md_stage_3_count[cand_class_it] &&
                        ((((*(context_ptr->candidate_buffer_ptr_array[cand_buff_indices[cand_count]]
                                  ->full_cost_ptr) -
                            class_best_cost) *
                           100) /
#if FTR_NIC_PRUNING
                          class_best_cost) < mds3_cand_th)) {
#else
                          class_best_cost) < context_ptr->md_stage_3_cand_prune_th)) {
#endif
                        cand_count++;
                    }
                context_ptr->md_stage_3_count[cand_class_it] = cand_count;
            }
        context_ptr->md_stage_3_total_count += context_ptr->md_stage_3_count[cand_class_it];
    }
}
#if !CLN_NSQ_AND_STATS
void distortion_based_modulator(ModeDecisionContext *context_ptr,
                                EbPictureBufferDesc *input_picture_ptr, uint32_t input_origin_index,
                                EbPictureBufferDesc *recon_ptr, uint32_t blk_origin_index) {
    if (context_ptr->blk_geom->shape == PART_N) {
        uint8_t shape_idx;
        for (shape_idx = 0; shape_idx < NUMBER_OF_SHAPES; shape_idx++)
            context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                .sse_gradian_band[shape_idx] = 1;
        uint32_t sq_size = context_ptr->blk_geom->sq_size;
        if (sq_size > 4) {
            uint8_t  r, c;
            uint64_t min_blk_dist[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};

            uint64_t part_dist[25]                          = {0};
            uint64_t mark_part_to_process[NUMBER_OF_SHAPES] = {0};

            int32_t min_size     = sq_size == 128 ? 64 : sq_size == 64 ? 16 : sq_size == 32 ? 8 : 4;
            int32_t min_size_num = sq_size / min_size;

            for (r = 0; r < min_size_num; r++) {
                for (c = 0; c < min_size_num; c++) {
                    int32_t min_blk_index = (int32_t)blk_origin_index +
                        ((c * min_size) + ((r * min_size) * recon_ptr->stride_y));
                    EbSpatialFullDistType spatial_full_dist_type_fun =
                        context_ptr->hbd_mode_decision ? svt_full_distortion_kernel16_bits
                                                       : svt_spatial_full_distortion_kernel;
                    min_blk_dist[r][c] = spatial_full_dist_type_fun(input_picture_ptr->buffer_y,
                                                                    input_origin_index,
                                                                    input_picture_ptr->stride_y,
                                                                    recon_ptr->buffer_y,
                                                                    min_blk_index,
                                                                    recon_ptr->stride_y,
                                                                    min_size,
                                                                    min_size);
                    part_dist[0] += min_blk_dist[r][c];
                }
            }
            if (sq_size == 64 || sq_size == 32 || sq_size == 16) {
                part_dist[1] = min_blk_dist[0][0] + min_blk_dist[0][1] + min_blk_dist[0][2] +
                    min_blk_dist[0][3] + min_blk_dist[1][0] + min_blk_dist[1][1] +
                    min_blk_dist[1][2] + min_blk_dist[1][3];
                part_dist[2] = min_blk_dist[2][0] + min_blk_dist[2][1] + min_blk_dist[2][2] +
                    min_blk_dist[2][3] + min_blk_dist[3][0] + min_blk_dist[3][1] +
                    min_blk_dist[3][2] + min_blk_dist[3][3];
                part_dist[3] = min_blk_dist[0][0] + min_blk_dist[1][0] + min_blk_dist[2][0] +
                    min_blk_dist[3][0] + min_blk_dist[0][1] + min_blk_dist[1][1] +
                    min_blk_dist[2][1] + min_blk_dist[3][1];
                part_dist[4] = min_blk_dist[0][2] + min_blk_dist[1][2] + min_blk_dist[2][2] +
                    min_blk_dist[3][2] + min_blk_dist[0][3] + min_blk_dist[1][3] +
                    min_blk_dist[2][3] + min_blk_dist[3][3];
                part_dist[5] = min_blk_dist[0][0] + min_blk_dist[0][1] + min_blk_dist[1][0] +
                    min_blk_dist[1][1];
                part_dist[6] = min_blk_dist[0][2] + min_blk_dist[0][3] + min_blk_dist[1][2] +
                    min_blk_dist[1][3];
                part_dist[7] = part_dist[2];
                part_dist[8] = part_dist[1];
                part_dist[9] = min_blk_dist[2][0] + min_blk_dist[2][1] + min_blk_dist[3][0] +
                    min_blk_dist[3][1];
                part_dist[10] = min_blk_dist[2][2] + min_blk_dist[2][3] + min_blk_dist[3][2] +
                    min_blk_dist[3][3];
                part_dist[11] = part_dist[5];
                part_dist[12] = part_dist[9];
                part_dist[13] = part_dist[4];
                part_dist[14] = part_dist[3];
                part_dist[15] = part_dist[6];
                part_dist[16] = part_dist[10];
                part_dist[17] = min_blk_dist[0][0] + min_blk_dist[0][1] + min_blk_dist[0][2] +
                    min_blk_dist[0][3];
                part_dist[18] = min_blk_dist[1][0] + min_blk_dist[1][1] + min_blk_dist[1][2] +
                    min_blk_dist[1][3];
                part_dist[19] = min_blk_dist[2][0] + min_blk_dist[2][1] + min_blk_dist[2][2] +
                    min_blk_dist[2][3];
                part_dist[20] = min_blk_dist[3][0] + min_blk_dist[3][1] + min_blk_dist[3][2] +
                    min_blk_dist[3][3];
                part_dist[21] = min_blk_dist[0][0] + min_blk_dist[1][0] + min_blk_dist[2][0] +
                    min_blk_dist[3][0];
                part_dist[22] = min_blk_dist[0][1] + min_blk_dist[1][1] + min_blk_dist[2][1] +
                    min_blk_dist[3][1];
                part_dist[23] = min_blk_dist[0][2] + min_blk_dist[1][2] + min_blk_dist[2][2] +
                    min_blk_dist[3][2];
                part_dist[24] = min_blk_dist[0][3] + min_blk_dist[1][3] + min_blk_dist[2][3] +
                    min_blk_dist[3][3];

                // PART_H decision
                uint64_t min_dist;
                uint8_t  part_idx;
                uint8_t  min_idx             = part_dist[1] < part_dist[2] ? 1 : 2;
                uint8_t  max_idx             = part_dist[1] < part_dist[2] ? 2 : 1;
                uint64_t distance_dist       = part_dist[max_idx] - part_dist[min_idx];
                uint64_t per                 = part_dist[min_idx]
                                    ? MIN(1000, (distance_dist * 100 / part_dist[min_idx]))
                                    : 1000;
                mark_part_to_process[PART_H] = per;

                // PART_V decision
                min_idx       = part_dist[3] < part_dist[4] ? 3 : 4;
                max_idx       = part_dist[3] < part_dist[4] ? 4 : 3;
                distance_dist = part_dist[max_idx] - part_dist[min_idx];
                per = part_dist[min_idx] ? MIN(1000, (distance_dist * 100 / part_dist[min_idx]))
                                         : 1000;
                mark_part_to_process[PART_V] = per;

                // PART_HA decision
                min_idx       = part_dist[5] < part_dist[6] ? 5 : 6;
                max_idx       = part_dist[5] < part_dist[6] ? 6 : 5;
                distance_dist = part_dist[max_idx] - part_dist[min_idx];
                per = part_dist[min_idx] ? MIN(1000, (distance_dist * 100 / part_dist[min_idx]))
                                         : 1000;
                mark_part_to_process[PART_HA] = per;

                // PART_HB decision
                min_idx       = part_dist[9] < part_dist[10] ? 9 : 10;
                max_idx       = part_dist[9] < part_dist[10] ? 10 : 9;
                distance_dist = part_dist[max_idx] - part_dist[min_idx];
                per = part_dist[min_idx] ? MIN(1000, (distance_dist * 100 / part_dist[min_idx]))
                                         : 1000;
                mark_part_to_process[PART_HB] = per;

                // PART_VA decision
                min_idx       = part_dist[11] < part_dist[12] ? 11 : 12;
                max_idx       = part_dist[11] < part_dist[12] ? 12 : 11;
                distance_dist = part_dist[max_idx] - part_dist[min_idx];
                per = part_dist[min_idx] ? MIN(1000, (distance_dist * 100 / part_dist[min_idx]))
                                         : 1000;
                mark_part_to_process[PART_VA] = per;

                // PART_VB decision
                min_idx       = part_dist[15] < part_dist[16] ? 15 : 16;
                max_idx       = part_dist[15] < part_dist[16] ? 16 : 15;
                distance_dist = part_dist[max_idx] - part_dist[min_idx];
                per = part_dist[min_idx] ? MIN(1000, (distance_dist * 100 / part_dist[min_idx]))
                                         : 1000;
                mark_part_to_process[PART_VB] = per;

                // PART_H4 decision
                min_dist = MIN(part_dist[17],
                               MIN(part_dist[18], MIN(part_dist[19], part_dist[20])));
                for (part_idx = 17; part_idx <= 20; part_idx++) {
                    distance_dist = part_dist[part_idx] - min_dist;
                    per           = min_dist ? MIN(1000, (distance_dist * 100 / min_dist)) : 1000;
                    mark_part_to_process[PART_H4] = MAX(mark_part_to_process[PART_H4], per);
                }
                // PART_V4 decision
                min_dist = MIN(part_dist[21],
                               MIN(part_dist[22], MIN(part_dist[23], part_dist[24])));
                for (part_idx = 21; part_idx <= 24; part_idx++) {
                    distance_dist = part_dist[part_idx] - min_dist;
                    per           = min_dist ? MIN(1000, (distance_dist * 100 / min_dist)) : 1000;
                    mark_part_to_process[PART_V4] = MAX(mark_part_to_process[PART_V4], per);
                }

            } else {
                part_dist[1]  = min_blk_dist[0][0] + min_blk_dist[0][1];
                part_dist[2]  = min_blk_dist[1][0] + min_blk_dist[1][1];
                part_dist[3]  = min_blk_dist[0][0] + min_blk_dist[1][0];
                part_dist[4]  = min_blk_dist[0][1] + min_blk_dist[1][1];
                part_dist[5]  = min_blk_dist[0][0];
                part_dist[6]  = min_blk_dist[0][1];
                part_dist[7]  = part_dist[2];
                part_dist[8]  = part_dist[1];
                part_dist[9]  = min_blk_dist[1][0];
                part_dist[10] = min_blk_dist[1][1];
                part_dist[11] = part_dist[5];
                part_dist[12] = part_dist[9];
                part_dist[13] = part_dist[4];
                part_dist[14] = part_dist[3];
                part_dist[15] = part_dist[6];
                part_dist[16] = part_dist[10];

                // PART_H decision
                uint8_t  min_idx             = part_dist[1] < part_dist[2] ? 1 : 2;
                uint8_t  max_idx             = part_dist[1] < part_dist[2] ? 2 : 1;
                uint64_t distance_dist       = part_dist[max_idx] - part_dist[min_idx];
                uint64_t per                 = part_dist[min_idx]
                                    ? MIN(1000, (distance_dist * 100 / part_dist[min_idx]))
                                    : 1000;
                mark_part_to_process[PART_H] = per;

                // PART_V decision
                min_idx       = part_dist[3] < part_dist[4] ? 3 : 4;
                max_idx       = part_dist[3] < part_dist[4] ? 4 : 3;
                distance_dist = part_dist[max_idx] - part_dist[min_idx];
                per = part_dist[min_idx] ? MIN(1000, (distance_dist * 100 / part_dist[min_idx]))
                                         : 1000;
                mark_part_to_process[PART_V] = per;
                if (sq_size == 128) {
                    // PART_HA decision
                    min_idx       = part_dist[5] < part_dist[6] ? 5 : 6;
                    max_idx       = part_dist[5] < part_dist[6] ? 6 : 5;
                    distance_dist = part_dist[max_idx] - part_dist[min_idx];
                    per = part_dist[min_idx] ? MIN(1000, (distance_dist * 100 / part_dist[min_idx]))
                                             : 1000;
                    mark_part_to_process[PART_HA] = per;

                    // PART_HB decision
                    min_idx       = part_dist[9] < part_dist[10] ? 9 : 10;
                    max_idx       = part_dist[9] < part_dist[10] ? 10 : 9;
                    distance_dist = part_dist[max_idx] - part_dist[min_idx];
                    per = part_dist[min_idx] ? MIN(1000, (distance_dist * 100 / part_dist[min_idx]))
                                             : 1000;
                    mark_part_to_process[PART_HB] = per;

                    // PART_VA decision
                    min_idx       = part_dist[11] < part_dist[12] ? 11 : 12;
                    max_idx       = part_dist[11] < part_dist[12] ? 12 : 11;
                    distance_dist = part_dist[max_idx] - part_dist[min_idx];
                    per = part_dist[min_idx] ? MIN(1000, (distance_dist * 100 / part_dist[min_idx]))
                                             : 1000;
                    mark_part_to_process[PART_VA] = per;

                    // PART_VB decision
                    min_idx       = part_dist[15] < part_dist[16] ? 15 : 16;
                    max_idx       = part_dist[15] < part_dist[16] ? 16 : 15;
                    distance_dist = part_dist[max_idx] - part_dist[min_idx];
                    per = part_dist[min_idx] ? MIN(1000, (distance_dist * 100 / part_dist[min_idx]))
                                             : 1000;
                    mark_part_to_process[PART_VB] = per;
                }
            }
            for (shape_idx = 0; shape_idx < NUMBER_OF_SHAPES; shape_idx++) {
                if (mark_part_to_process[shape_idx] < 10)
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                        .sse_gradian_band[shape_idx] = 0;
                else
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                        .sse_gradian_band[shape_idx] = 1;
            }
        }
    }
}
#endif
#if FTR_REF_BITS
uint64_t estimate_ref_frame_type_bits(PictureControlSet *pcs_ptr, ModeDecisionContext *ctx,
                                      BlkStruct *blk_ptr, uint32_t bwidth, uint32_t bheight,
                                      uint8_t ref_frame_type, EbBool is_compound);
/*
 * Estimate the rate of signaling all available ref_frame_type
 */
void estimate_ref_frames_num_bits(struct ModeDecisionContext *context_ptr,
                                  PictureControlSet *         pcs_ptr) {
    for (uint32_t ref_it = 0; ref_it < pcs_ptr->parent_pcs_ptr->tot_ref_frame_types; ++ref_it) {
        MvReferenceFrame ref_pair = pcs_ptr->parent_pcs_ptr->ref_frame_type_arr[ref_it];
        MvReferenceFrame rf[2];
        av1_set_ref_frame(rf, ref_pair);

        //single ref/list
        if (rf[1] == NONE_FRAME) {
            MvReferenceFrame ref_frame_type = rf[0];
            context_ptr->estimate_ref_frames_num_bits[ref_frame_type][0] =
                estimate_ref_frame_type_bits(pcs_ptr,
                                             context_ptr,
                                             context_ptr->blk_ptr,
                                             context_ptr->blk_geom->bwidth,
                                             context_ptr->blk_geom->bheight,
                                             ref_frame_type,
                                             0);
        } else {
            uint8_t ref_idx_0  = get_ref_frame_idx(rf[0]);
            uint8_t ref_idx_1  = get_ref_frame_idx(rf[1]);
            uint8_t list_idx_0 = get_list_idx(rf[0]);
            uint8_t list_idx_1 = get_list_idx(rf[1]);

            MvReferenceFrame ref_frame_type = av1_ref_frame_type((const MvReferenceFrame[]){
                svt_get_ref_frame_type(list_idx_0, ref_idx_0),
                svt_get_ref_frame_type(list_idx_1, ref_idx_1),
            });

            context_ptr->estimate_ref_frames_num_bits[ref_frame_type][1] =
                estimate_ref_frame_type_bits(pcs_ptr,
                                             context_ptr,
                                             context_ptr->blk_ptr,
                                             context_ptr->blk_geom->bwidth,
                                             context_ptr->blk_geom->bheight,
                                             ref_frame_type,
                                             1);
        }
    }
}
#endif

#if FTR_NSQ_RED_USING_RECON
#if FTR_MODULATE_SRC_REC_TH
void calc_scr_to_recon_dist_per_quadrant(ModeDecisionContext *        context_ptr,
                                         EbPictureBufferDesc *        input_picture_ptr,
                                         const uint32_t               input_origin_index,
                                         const uint32_t               input_cb_origin_in_index,
                                         ModeDecisionCandidateBuffer *candidate_buffer,
                                         const uint32_t               blk_origin_index,
                                         const uint32_t               blk_chroma_origin_index) {
#else
void calc_scr_to_recon_dist_per_quadrant(ModeDecisionContext *context_ptr,
                                         EbPictureBufferDesc *input_picture_ptr,
                                         const uint32_t input_origin_index,
                                         ModeDecisionCandidateBuffer *candidate_buffer,
                                         const uint32_t blk_origin_index) {
#endif
#if FTR_IMPROVE_DEPTH_REMOVAL
#if LOWER_DEPTH_EXIT_CTRL
    if (context_ptr->lower_depth_block_skip_ctrls.enabled || (!context_ptr->md_disallow_nsq && context_ptr->max_part0_to_part1_dev)) {
#else
    if (context_ptr->depth_skip_ctrls.enabled ||
        (!context_ptr->md_disallow_nsq && context_ptr->max_part0_to_part1_dev)) {
#endif
#else
    if (!context_ptr->md_disallow_nsq) {
        if (context_ptr->max_part0_to_part1_dev) {
#endif
        // if a non-4x4 SQ
        if ((context_ptr->blk_geom->bwidth == context_ptr->blk_geom->bheight) &&
            context_ptr->blk_geom->sq_size > 4) {
            EbPictureBufferDesc *recon_ptr = candidate_buffer->recon_ptr;

            EbSpatialFullDistType spatial_full_dist_type_fun = context_ptr->hbd_mode_decision
                ? svt_full_distortion_kernel16_bits
                : svt_spatial_full_distortion_kernel;

            uint8_t r, c;
            int32_t quadrant_size = context_ptr->blk_geom->sq_size >> 1;

            for (r = 0; r < 2; r++) {
                for (c = 0; c < 2; c++) {
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                        .rec_dist_per_quadrant[c + (r << 1)] = spatial_full_dist_type_fun(
                        input_picture_ptr->buffer_y,
                        input_origin_index + c * quadrant_size +
                            (r * quadrant_size) * input_picture_ptr->stride_y,
                        input_picture_ptr->stride_y,
                        recon_ptr->buffer_y,
                        blk_origin_index + c * quadrant_size +
                            (r * quadrant_size) * recon_ptr->stride_y,
                        recon_ptr->stride_y,
                        (uint32_t)quadrant_size,
                        (uint32_t)quadrant_size);
#if FTR_MODULATE_SRC_REC_TH
                    // If quadrant_size == 4 then rec_dist_per_quadrant will have luma only because spatial_full_dist_type_fun does not support smaller than 4x4
                    if (context_ptr->blk_geom->has_uv &&
                        context_ptr->chroma_level <= CHROMA_MODE_1 && quadrant_size > 4) {
                        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                            .rec_dist_per_quadrant[c + (r << 1)] += spatial_full_dist_type_fun(
                            input_picture_ptr->buffer_cb,
                            input_cb_origin_in_index + c * (quadrant_size >> 1) +
                                (r * (quadrant_size >> 1)) * input_picture_ptr->stride_cb,
                            input_picture_ptr->stride_cb,
                            recon_ptr->buffer_cb,
                            blk_chroma_origin_index + c * (quadrant_size >> 1) +
                                (r * (quadrant_size >> 1)) * recon_ptr->stride_cb,
                            recon_ptr->stride_cb,
                            (uint32_t)(quadrant_size >> 1),
                            (uint32_t)(quadrant_size >> 1));

                        context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                            .rec_dist_per_quadrant[c + (r << 1)] += spatial_full_dist_type_fun(
                            input_picture_ptr->buffer_cr,
                            input_cb_origin_in_index + c * (quadrant_size >> 1) +
                                (r * (quadrant_size >> 1)) * input_picture_ptr->stride_cr,
                            input_picture_ptr->stride_cr,
                            recon_ptr->buffer_cr,
                            blk_chroma_origin_index + c * (quadrant_size >> 1) +
                                (r * (quadrant_size >> 1)) * recon_ptr->stride_cr,
                            recon_ptr->stride_cr,
                            (uint32_t)(quadrant_size >> 1),
                            (uint32_t)(quadrant_size >> 1));
                    }
#endif
                }
            }
        }
#if FTR_IMPROVE_DEPTH_REMOVAL
    }
#else
        }
    }
#endif
}
#endif

void md_encode_block(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
#if OPT_SB_CLASS
#if OPT6_DEPTH_REFINEMENT
                     const uint8_t blk_split_flag,
#else
                     const EbMdcLeafData *const leaf_data_ptr,
#endif
#endif
#if RFCTR_MD_BLOCK_LOOP
                     EbPictureBufferDesc *input_picture_ptr) {
#else
                     EbPictureBufferDesc *input_picture_ptr,
                     ModeDecisionCandidateBuffer *bestcandidate_buffers[5]) {
#endif
    ModeDecisionCandidateBuffer **candidate_buffer_ptr_array_base =
        context_ptr->candidate_buffer_ptr_array;
    ModeDecisionCandidateBuffer **candidate_buffer_ptr_array;
    const BlockGeom *             blk_geom = context_ptr->blk_geom;

    const uint32_t input_cb_origin_in_index = ((context_ptr->round_origin_y >> 1) +
                                               (input_picture_ptr->origin_y >> 1)) *
            input_picture_ptr->stride_cb +
        ((context_ptr->round_origin_x >> 1) + (input_picture_ptr->origin_x >> 1));
    const uint32_t blk_chroma_origin_index = ROUND_UV(blk_geom->origin_x) / 2 +
        ROUND_UV(blk_geom->origin_y) / 2 * (context_ptr->sb_size >> 1);
    BlkStruct *blk_ptr             = context_ptr->blk_ptr;
    candidate_buffer_ptr_array     = &(candidate_buffer_ptr_array_base[0]);
    context_ptr->blk_lambda_tuning = pcs_ptr->parent_pcs_ptr->blk_lambda_tuning;
    //Get the new lambda for current block
    if (pcs_ptr->parent_pcs_ptr->blk_lambda_tuning) {
        set_tuned_blk_lambda(context_ptr, pcs_ptr);
    }


#if OPT_INIT_XD // need to init xd before product_coding_loop_init_fast_loop()
    init_xd(pcs_ptr, context_ptr);
    if (!context_ptr->shut_fast_rate) {
        FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;
        // Generate MVP(s)
        if (frm_hdr->allow_intrabc) { // pcs_ptr->slice_type == I_SLICE
            MvReferenceFrame ref_frame = INTRA_FRAME;
            generate_av1_mvp_table(context_ptr,
                context_ptr->blk_ptr,
                context_ptr->blk_geom,
                context_ptr->blk_origin_x,
                context_ptr->blk_origin_y,
                &ref_frame,
                1,
                pcs_ptr);
        }
        else if (pcs_ptr->slice_type != I_SLICE) {
            generate_av1_mvp_table(context_ptr,
                context_ptr->blk_ptr,
                context_ptr->blk_geom,
                context_ptr->blk_origin_x,
                context_ptr->blk_origin_y,
                pcs_ptr->parent_pcs_ptr->ref_frame_type_arr,
                pcs_ptr->parent_pcs_ptr->tot_ref_frame_types,
                pcs_ptr);
        }
    }
#else
    blk_ptr->av1xd->tile.mi_col_start = context_ptr->sb_ptr->tile_info.mi_col_start;
    blk_ptr->av1xd->tile.mi_col_end   = context_ptr->sb_ptr->tile_info.mi_col_end;
    blk_ptr->av1xd->tile.mi_row_start = context_ptr->sb_ptr->tile_info.mi_row_start;
    blk_ptr->av1xd->tile.mi_row_end   = context_ptr->sb_ptr->tile_info.mi_row_end;
#endif

#if OPT_INIT_XD
    product_coding_loop_init_fast_loop(pcs_ptr,
                                       context_ptr,
                                       context_ptr->skip_flag_neighbor_array,
                                       context_ptr->mode_type_neighbor_array,
                                       context_ptr->leaf_partition_neighbor_array);
#else
    product_coding_loop_init_fast_loop(context_ptr,
#if !CLN_MDC_CTX
                                       context_ptr->inter_pred_dir_neighbor_array,
                                       context_ptr->ref_frame_type_neighbor_array,
#endif
#if !CLN_MDC_CTX
                                       context_ptr->intra_luma_mode_neighbor_array,
#endif
                                       context_ptr->skip_flag_neighbor_array,
                                       context_ptr->mode_type_neighbor_array,
                                       context_ptr->leaf_partition_neighbor_array);
#endif

    // Initialize uv_search_path
    if (context_ptr->chroma_at_last_md_stage) {
        if (context_ptr->blk_geom->sq_size < 128) {
            if (context_ptr->blk_geom->has_uv) {
                init_chroma_mode(context_ptr);
            }
        }
    } else {
        // Search the best independent intra chroma mode
        if (context_ptr->chroma_level == CHROMA_MODE_0) {
            if (context_ptr->blk_geom->sq_size < 128) {
                if (context_ptr->blk_geom->has_uv) {
                    search_best_independent_uv_mode(pcs_ptr,
                                                    input_picture_ptr,
                                                    input_cb_origin_in_index,
                                                    input_cb_origin_in_index,
                                                    blk_chroma_origin_index,
                                                    context_ptr);
                }
            }
        }
    }
#if !OPT_INIT_XD
    FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;
    // Generate MVP(s)
    if (!context_ptr->shut_fast_rate) {
        if (frm_hdr->allow_intrabc) // pcs_ptr->slice_type == I_SLICE
#if FIX_SC_MVP_TABLE_GEN
        {
            MvReferenceFrame ref_frame = INTRA_FRAME;
            generate_av1_mvp_table(&context_ptr->sb_ptr->tile_info,
                                   context_ptr,
                                   context_ptr->blk_ptr,
                                   context_ptr->blk_geom,
                                   context_ptr->blk_origin_x,
                                   context_ptr->blk_origin_y,
                                   &ref_frame,
                                   1,
                                   pcs_ptr);

        } else if (pcs_ptr->slice_type != I_SLICE) {
            generate_av1_mvp_table(&context_ptr->sb_ptr->tile_info,
                                   context_ptr,
                                   context_ptr->blk_ptr,
                                   context_ptr->blk_geom,
                                   context_ptr->blk_origin_x,
                                   context_ptr->blk_origin_y,
                                   pcs_ptr->parent_pcs_ptr->ref_frame_type_arr,
                                   pcs_ptr->parent_pcs_ptr->tot_ref_frame_types,
                                   pcs_ptr);
        }
#else
            generate_av1_mvp_table(&context_ptr->sb_ptr->tile_info,
                                   context_ptr,
                                   context_ptr->blk_ptr,
                                   context_ptr->blk_geom,
                                   context_ptr->blk_origin_x,
                                   context_ptr->blk_origin_y,
                                   pcs_ptr->parent_pcs_ptr->ref_frame_type_arr,
                                   1,
                                   pcs_ptr);
    } else if (pcs_ptr->slice_type != I_SLICE)
        generate_av1_mvp_table(&context_ptr->sb_ptr->tile_info,
                               context_ptr,
                               context_ptr->blk_ptr,
                               context_ptr->blk_geom,
                               context_ptr->blk_origin_x,
                               context_ptr->blk_origin_y,
                               pcs_ptr->parent_pcs_ptr->ref_frame_type_arr,
                               pcs_ptr->parent_pcs_ptr->tot_ref_frame_types,
                               pcs_ptr);
#endif
    } else {
        init_xd(pcs_ptr, context_ptr);
    }
#endif
    // Read MVPs (rounded-up to the closest integer) for use in md_sq_motion_search() and/or predictive_me_search() and/or perform_md_reference_pruning()
    if (pcs_ptr->slice_type != I_SLICE &&
        (context_ptr->md_sq_me_ctrls.enabled || context_ptr->md_pme_ctrls.enabled ||
         context_ptr->ref_pruning_ctrls.enabled || context_ptr->md_subpel_me_ctrls.enabled ||
         context_ptr->md_subpel_pme_ctrls.enabled))
        build_single_ref_mvp_array(pcs_ptr, context_ptr);
    if (pcs_ptr->slice_type != I_SLICE)
        // Read and (if needed) perform 1/8 Pel ME MVs refinement
        read_refine_me_mvs(pcs_ptr, context_ptr, input_picture_ptr);
#if OPT_PME_RES_PREP
    // Initialized for eliminate_candidate_based_on_pme_me_results()
    context_ptr->pme_res[0][0].dist = context_ptr->pme_res[1][0].dist = 0xFFFFFFFF;
#else
#if FTR_PD2_REDUCE_MDS0
for (uint32_t li = 0; li < MAX_NUM_OF_REF_PIC_LIST; ++li) {
    for (uint32_t ri = 0; ri < REF_LIST_MAX_DEPTH; ++ri) {
        context_ptr->pme_res[li][ri].dist = 0xFFFFFFFF;
        context_ptr->pme_res[li][ri].list_i = li;
        context_ptr->pme_res[li][ri].ref_i = ri;
    }
}
#endif
#endif
    // Perform md reference pruning
    if (context_ptr->ref_pruning_ctrls.enabled)
        perform_md_reference_pruning(pcs_ptr, context_ptr, input_picture_ptr);
    // Perform ME search around the best MVP
    if (context_ptr->md_pme_ctrls.enabled) {
#if !CLN_INIT_OP
        for (uint32_t li = 0; li < MAX_NUM_OF_REF_PIC_LIST; ++li) {
            for (uint32_t ri = 0; ri < REF_LIST_MAX_DEPTH; ++ri) {
                context_ptr->pme_res[li][ri].dist   = 0xFFFFFFFF;
                context_ptr->pme_res[li][ri].list_i = li;
                context_ptr->pme_res[li][ri].ref_i  = ri;
            }
        }
#endif
        pme_search(pcs_ptr, context_ptr, input_picture_ptr);
    }

    if (context_ptr->md_inter_intra_level) {
        int allow_ii = is_interintra_allowed_bsize(context_ptr->blk_geom->bsize);
        if (allow_ii)
            precompute_intra_pred_for_inter_intra(pcs_ptr, context_ptr);
    }

    uint32_t                      fast_candidate_total_count;
    generate_md_stage_0_cand(
        context_ptr->sb_ptr, context_ptr, &fast_candidate_total_count, pcs_ptr);
#if FTR_REF_BITS
    if (pcs_ptr->slice_type != I_SLICE) {
        if (!context_ptr->shut_fast_rate) {
            estimate_ref_frames_num_bits(context_ptr, pcs_ptr);
        }
    }
#endif
    CandClass cand_class_it;
    uint32_t  buffer_start_idx = 0;
    uint32_t  buffer_count_for_curr_class;
    uint32_t  buffer_total_count        = 0;
    context_ptr->md_stage_1_total_count = 0;
    context_ptr->md_stage_2_total_count = 0;
    context_ptr->md_stage_3_total_count = 0;
    // Derive NIC(s)
    set_md_stage_counts(pcs_ptr, context_ptr);
    uint64_t best_md_stage_cost     = (uint64_t)~0;
    context_ptr->md_stage           = MD_STAGE_0;
    uint8_t best_md_stage_pred_mode = 0;
#if FTR_REDUCE_MDS2_CAND
    context_ptr->mds0_best_idx      = 0;
    context_ptr->mds0_best_class_it = 0;
    context_ptr->mds1_best_idx      = 0;
    context_ptr->mds1_best_class_it = 0;
#endif
    const uint32_t input_origin_index = (context_ptr->blk_origin_y + input_picture_ptr->origin_y) *
        input_picture_ptr->stride_y +
        (context_ptr->blk_origin_x + input_picture_ptr->origin_x);
    const uint32_t blk_origin_index = blk_geom->origin_x +
        blk_geom->origin_y * context_ptr->sb_size;
    for (cand_class_it = CAND_CLASS_0; cand_class_it < CAND_CLASS_TOTAL; cand_class_it++) {
        //number of next level candidates could not exceed number of curr level candidates
        context_ptr->md_stage_1_count[cand_class_it] = MIN(
            context_ptr->md_stage_0_count[cand_class_it],
            context_ptr->md_stage_1_count[cand_class_it]);

        if (context_ptr->md_stage_0_count[cand_class_it] > 0 &&
            context_ptr->md_stage_1_count[cand_class_it] > 0) {
            buffer_count_for_curr_class = context_ptr->md_stage_0_count[cand_class_it] >
                    context_ptr->md_stage_1_count[cand_class_it]
                ? (context_ptr->md_stage_1_count[cand_class_it] + 1)
                : context_ptr->md_stage_1_count[cand_class_it];

            buffer_total_count += buffer_count_for_curr_class;
#if CLN_MD_CAND_BUFF
            assert_err(buffer_total_count <= context_ptr->max_nics, "not enough cand buffers");
#else
        assert(buffer_total_count <= MAX_NFL_BUFF && "not enough cand buffers");
#endif
            //Input: md_stage_0_count[cand_class_it]  Output:  md_stage_1_count[cand_class_it]
            context_ptr->target_class = cand_class_it;
            md_stage_0(
                pcs_ptr,
                context_ptr,
                candidate_buffer_ptr_array_base,
                context_ptr->fast_candidate_array,
                0,
                fast_candidate_total_count - 1,
                input_picture_ptr,
                input_origin_index,
                input_cb_origin_in_index,
                input_cb_origin_in_index,
                blk_ptr,
                blk_origin_index,
                blk_chroma_origin_index,
                buffer_start_idx,
                buffer_count_for_curr_class,
                context_ptr->md_stage_0_count[cand_class_it] >
                    context_ptr
                        ->md_stage_1_count[cand_class_it]); //is there need to max the temp buffer

            //Sort:  md_stage_1_count[cand_class_it]
            memset(context_ptr->cand_buff_indices[cand_class_it],
                   0xFF,
                   context_ptr->md_stage_3_total_count * sizeof(uint32_t));
            sort_fast_cost_based_candidates(
                context_ptr,
                buffer_start_idx,
                buffer_count_for_curr_class, //how many cand buffers to sort. one of the buffers can have max cost.
                context_ptr->cand_buff_indices[cand_class_it]);
            uint32_t *cand_buff_indices = context_ptr->cand_buff_indices[cand_class_it];
            if (*(context_ptr->candidate_buffer_ptr_array[cand_buff_indices[0]]->fast_cost_ptr) <
                best_md_stage_cost) {
                best_md_stage_pred_mode =
                    (context_ptr->candidate_buffer_ptr_array[cand_buff_indices[0]]->candidate_ptr)
                        ->pred_mode;
                best_md_stage_cost = *(
                    context_ptr->candidate_buffer_ptr_array[cand_buff_indices[0]]->fast_cost_ptr);
#if FTR_REDUCE_MDS2_CAND
                context_ptr->mds0_best_idx      = cand_buff_indices[0];
                context_ptr->mds0_best_class_it = cand_class_it;
#endif
            }

            buffer_start_idx += buffer_count_for_curr_class; //for next iteration.
        }
    }
#if OPT_LOSSLESS_1
    if (context_ptr->use_best_mds0) {
        for (cand_class_it = CAND_CLASS_0; cand_class_it < CAND_CLASS_TOTAL; cand_class_it++) {
            //number of next level candidates could not exceed number of curr level candidates
            context_ptr->md_stage_3_count[cand_class_it] = MIN(
                context_ptr->md_stage_1_count[cand_class_it],
                context_ptr->md_stage_3_count[cand_class_it]);
        }
        context_ptr->md_stage_3_total_count = 1;
    } else {
#endif
        interintra_class_pruning_1(context_ptr, best_md_stage_cost, best_md_stage_pred_mode);

        // 1st Full-Loop
        best_md_stage_cost    = (uint64_t)~0;
        context_ptr->md_stage = MD_STAGE_1;
        for (cand_class_it = CAND_CLASS_0; cand_class_it < CAND_CLASS_TOTAL; cand_class_it++) {
            //number of next level candidates could not exceed number of curr level candidates
            context_ptr->md_stage_2_count[cand_class_it] = MIN(
                context_ptr->md_stage_1_count[cand_class_it],
                context_ptr->md_stage_2_count[cand_class_it]);
            if (context_ptr->bypass_md_stage_1[cand_class_it] == EB_FALSE &&
                context_ptr->md_stage_1_count[cand_class_it] > 0 &&
                context_ptr->md_stage_2_count[cand_class_it] > 0) {
                context_ptr->target_class = cand_class_it;
                md_stage_1(pcs_ptr,
                           context_ptr->sb_ptr,
                           blk_ptr,
                           context_ptr,
                           input_picture_ptr,
                           input_origin_index,
                           input_cb_origin_in_index,
                           blk_origin_index,
                           blk_chroma_origin_index);

                // Sort the candidates of the target class based on the 1st full loop cost

                //sort the new set of candidates
                if (context_ptr->md_stage_1_count[cand_class_it])
                    sort_full_cost_based_candidates(context_ptr,
                                                    context_ptr->md_stage_1_count[cand_class_it],
                                                    context_ptr->cand_buff_indices[cand_class_it]);
                uint32_t *cand_buff_indices = context_ptr->cand_buff_indices[cand_class_it];
#if FTR_REDUCE_MDS2_CAND
                if (*(context_ptr->candidate_buffer_ptr_array[cand_buff_indices[0]]
                          ->full_cost_ptr) < best_md_stage_cost) {
                    best_md_stage_cost = *(
                        context_ptr->candidate_buffer_ptr_array[cand_buff_indices[0]]
                            ->full_cost_ptr);
                    context_ptr->mds1_best_idx      = cand_buff_indices[0];
                    context_ptr->mds1_best_class_it = cand_class_it;
                }
#else
        best_md_stage_cost = MIN(
            (*(context_ptr->candidate_buffer_ptr_array[cand_buff_indices[0]]->full_cost_ptr)),
            best_md_stage_cost);
#endif
            }
        }
        interintra_class_pruning_2(pcs_ptr, context_ptr, best_md_stage_cost);
        // 2nd Full-Loop
        best_md_stage_cost    = (uint64_t)~0;
        context_ptr->md_stage = MD_STAGE_2;
        for (cand_class_it = CAND_CLASS_0; cand_class_it < CAND_CLASS_TOTAL; cand_class_it++) {
            //number of next level candidates could not exceed number of curr level candidates
            context_ptr->md_stage_3_count[cand_class_it] = MIN(
                context_ptr->md_stage_2_count[cand_class_it],
                context_ptr->md_stage_3_count[cand_class_it]);

            if (context_ptr->bypass_md_stage_2[cand_class_it] == EB_FALSE &&
                context_ptr->md_stage_2_count[cand_class_it] > 0 &&
                context_ptr->md_stage_3_count[cand_class_it] > 0) {
                context_ptr->target_class = cand_class_it;

                md_stage_2(pcs_ptr,
                           context_ptr->sb_ptr,
                           blk_ptr,
                           context_ptr,
                           input_picture_ptr,
                           input_origin_index,
                           input_cb_origin_in_index,
                           blk_origin_index,
                           blk_chroma_origin_index);

                // Sort the candidates of the target class based on the 1st full loop cost

                //sort the new set of candidates
                if (context_ptr->md_stage_2_count[cand_class_it])
                    sort_full_cost_based_candidates(context_ptr,
                                                    context_ptr->md_stage_2_count[cand_class_it],
                                                    context_ptr->cand_buff_indices[cand_class_it]);

                uint32_t *cand_buff_indices = context_ptr->cand_buff_indices[cand_class_it];
                best_md_stage_cost          = MIN(
                    (*(context_ptr->candidate_buffer_ptr_array[cand_buff_indices[0]]
                           ->full_cost_ptr)),
                    best_md_stage_cost);
            }
        }

        interintra_class_pruning_3(context_ptr, best_md_stage_cost);
#if OPT_LOSSLESS_1
    }
#endif
#if !CLN_MD_CAND_BUFF
    assert(context_ptr->md_stage_3_total_count <= MAX_NFL);
#endif
    assert(context_ptr->md_stage_3_total_count > 0);
    construct_best_sorted_arrays_md_stage_3(
        context_ptr, candidate_buffer_ptr_array, context_ptr->best_candidate_index_array);
    // Search the best independent intra chroma mode
    if (context_ptr->chroma_at_last_md_stage) {
        // Initialize uv_search_path
        if (context_ptr->blk_geom->sq_size < 128) {
            if (context_ptr->blk_geom->has_uv) {
                if (context_ptr->md_stage_3_total_intra_count)
                    search_best_independent_uv_mode(pcs_ptr,
                                                    input_picture_ptr,
                                                    input_cb_origin_in_index,
                                                    input_cb_origin_in_index,
                                                    blk_chroma_origin_index,
                                                    context_ptr);
            }
        }
    }

    // 3rd Full-Loop
    context_ptr->md_stage = MD_STAGE_3;
    md_stage_3(pcs_ptr,
               context_ptr->sb_ptr,
               blk_ptr,
               context_ptr,
               input_picture_ptr,
               input_origin_index,
               input_cb_origin_in_index,
               blk_origin_index,
               blk_chroma_origin_index,
               context_ptr->md_stage_3_total_count);

    // Full Mode Decision (choose the best mode)
    uint32_t candidate_index  = product_full_mode_decision(context_ptr,
                                                 blk_ptr,
                                                 candidate_buffer_ptr_array,
                                                 context_ptr->md_stage_3_total_count,
                                                 context_ptr->best_candidate_index_array);
    ModeDecisionCandidateBuffer * candidate_buffer = candidate_buffer_ptr_array[candidate_index];
#if !RFCTR_MD_BLOCK_LOOP
    bestcandidate_buffers[0] = candidate_buffer;
#endif
    uint8_t sq_index = svt_log2f(context_ptr->blk_geom->sq_size) - 2;
    if (context_ptr->blk_geom->shape == PART_N) {
        context_ptr->parent_sq_type[sq_index] = candidate_buffer->candidate_ptr->type;
#if !FTR_NEW_CYCLES_ALLOC
        context_ptr->parent_sq_has_coeff[sq_index] =
            (candidate_buffer->candidate_ptr->y_has_coeff ||
             candidate_buffer->candidate_ptr->u_has_coeff ||
             candidate_buffer->candidate_ptr->v_has_coeff)
            ? 1
            : 0;
#endif
        context_ptr->parent_sq_pred_mode[sq_index] = candidate_buffer->candidate_ptr->pred_mode;
    }
    if (!context_ptr->skip_intra)
        av1_perform_inverse_transform_recon(context_ptr, candidate_buffer, context_ptr->blk_geom);

#if FTR_NSQ_RED_USING_RECON
#if FTR_MODULATE_SRC_REC_TH
#if OPT_SB_CLASS
#if OPT6_DEPTH_REFINEMENT
    if (!context_ptr->md_disallow_nsq || blk_split_flag)
#else
    if (!context_ptr->md_disallow_nsq || leaf_data_ptr->split_flag)
#endif
#endif
        calc_scr_to_recon_dist_per_quadrant(context_ptr,
                                            input_picture_ptr,
                                            input_origin_index,
                                            input_cb_origin_in_index,
                                            candidate_buffer,
                                            blk_origin_index,
                                            blk_chroma_origin_index);
#endif
#else
calc_scr_to_recon_dist_per_quadrant(context_ptr, input_picture_ptr, input_origin_index,
                                    candidate_buffer, blk_origin_index);
#endif
#if OPT_LOSSLESS_1
    if (!context_ptr->skip_intra) {
#endif
        if (!context_ptr->blk_geom->has_uv) {
            // Store the luma data for 4x* and *x4 blocks to be used for CFL
            EbPictureBufferDesc *recon_ptr       = candidate_buffer->recon_ptr;
            uint32_t             rec_luma_offset = context_ptr->blk_geom->origin_x +
                context_ptr->blk_geom->origin_y * recon_ptr->stride_y;
            if (context_ptr->hbd_mode_decision) {
                for (uint32_t j = 0; j < context_ptr->blk_geom->bheight; ++j)
                    svt_memcpy(context_ptr->cfl_temp_luma_recon16bit + rec_luma_offset +
                                   j * recon_ptr->stride_y,
                               ((uint16_t *)recon_ptr->buffer_y) +
                                   (rec_luma_offset + j * recon_ptr->stride_y),
                               sizeof(uint16_t) * context_ptr->blk_geom->bwidth);
            } else {
                for (uint32_t j = 0; j < context_ptr->blk_geom->bheight; ++j)
                    svt_memcpy(
                        &context_ptr
                             ->cfl_temp_luma_recon[rec_luma_offset + j * recon_ptr->stride_y],
                        recon_ptr->buffer_y + rec_luma_offset + j * recon_ptr->stride_y,
                        context_ptr->blk_geom->bwidth);
            }
        }
#if OPT_LOSSLESS_1
    }
#endif
    //copy neigh recon data in blk_ptr
    EbPictureBufferDesc *recon_ptr       = candidate_buffer->recon_ptr;
    uint32_t             rec_luma_offset = context_ptr->blk_geom->origin_x +
        context_ptr->blk_geom->origin_y * recon_ptr->stride_y;

    uint32_t rec_cb_offset = ((((context_ptr->blk_geom->origin_x >> 3) << 3) +
                               ((context_ptr->blk_geom->origin_y >> 3) << 3) *
                                   candidate_buffer->recon_ptr->stride_cb) >>
                              1);
    uint32_t rec_cr_offset = ((((context_ptr->blk_geom->origin_x >> 3) << 3) +
                               ((context_ptr->blk_geom->origin_y >> 3) << 3) *
                                   candidate_buffer->recon_ptr->stride_cr) >>
                              1);

    if (!context_ptr->hbd_mode_decision) {
#if !CLN_NSQ_AND_STATS
        if (context_ptr->pd_pass == PD_PASS_0 && pcs_ptr->parent_pcs_ptr->disallow_nsq == EB_FALSE)
            distortion_based_modulator(
                context_ptr, input_picture_ptr, input_origin_index, recon_ptr, blk_origin_index);
#endif
        svt_memcpy(
            context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds].neigh_top_recon[0],
            recon_ptr->buffer_y + rec_luma_offset +
                (context_ptr->blk_geom->bheight - 1) * recon_ptr->stride_y,
            context_ptr->blk_geom->bwidth);
        if (context_ptr->blk_geom->has_uv && context_ptr->chroma_level <= CHROMA_MODE_1) {
            svt_memcpy(context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                           .neigh_top_recon[1],
                       recon_ptr->buffer_cb + rec_cb_offset +
                           (context_ptr->blk_geom->bheight_uv - 1) * recon_ptr->stride_cb,
                       context_ptr->blk_geom->bwidth_uv);
            svt_memcpy(context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                           .neigh_top_recon[2],
                       recon_ptr->buffer_cr + rec_cr_offset +
                           (context_ptr->blk_geom->bheight_uv - 1) * recon_ptr->stride_cr,
                       context_ptr->blk_geom->bwidth_uv);
        }

        for (uint32_t j = 0; j < context_ptr->blk_geom->bheight; ++j)
            context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                .neigh_left_recon[0][j] =
                recon_ptr->buffer_y[rec_luma_offset + context_ptr->blk_geom->bwidth - 1 +
                                    j * recon_ptr->stride_y];

        if (context_ptr->blk_geom->has_uv && context_ptr->chroma_level <= CHROMA_MODE_1) {
            for (uint32_t j = 0; j < context_ptr->blk_geom->bheight_uv; ++j) {
                context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                    .neigh_left_recon[1][j] =
                    recon_ptr->buffer_cb[rec_cb_offset + context_ptr->blk_geom->bwidth_uv - 1 +
                                         j * recon_ptr->stride_cb];
                context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                    .neigh_left_recon[2][j] =
                    recon_ptr->buffer_cr[rec_cr_offset + context_ptr->blk_geom->bwidth_uv - 1 +
                                         j * recon_ptr->stride_cr];
            }
        }
    } else {
        uint16_t sz = sizeof(uint16_t);
        svt_memcpy(
            context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                .neigh_top_recon_16bit[0],
            recon_ptr->buffer_y +
                sz * (rec_luma_offset + (context_ptr->blk_geom->bheight - 1) * recon_ptr->stride_y),
            sz * context_ptr->blk_geom->bwidth);
        if (context_ptr->blk_geom->has_uv && context_ptr->chroma_level <= CHROMA_MODE_1) {
            svt_memcpy(context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                           .neigh_top_recon_16bit[1],
                       recon_ptr->buffer_cb +
                           sz *
                               (rec_cb_offset +
                                (context_ptr->blk_geom->bheight_uv - 1) * recon_ptr->stride_cb),
                       sz * context_ptr->blk_geom->bwidth_uv);
            svt_memcpy(context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                           .neigh_top_recon_16bit[2],
                       recon_ptr->buffer_cr +
                           sz *
                               (rec_cr_offset +
                                (context_ptr->blk_geom->bheight_uv - 1) * recon_ptr->stride_cr),
                       sz * context_ptr->blk_geom->bwidth_uv);
        }

        for (uint32_t j = 0; j < context_ptr->blk_geom->bheight; ++j)
            context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                .neigh_left_recon_16bit[0][j] =
                ((uint16_t *)recon_ptr->buffer_y)[rec_luma_offset + context_ptr->blk_geom->bwidth -
                                                  1 + j * recon_ptr->stride_y];

        if (context_ptr->blk_geom->has_uv && context_ptr->chroma_level <= CHROMA_MODE_1) {
            for (uint32_t j = 0; j < context_ptr->blk_geom->bheight_uv; ++j) {
                context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                    .neigh_left_recon_16bit[1][j] =
                    ((uint16_t *)
                         recon_ptr->buffer_cb)[rec_cb_offset + context_ptr->blk_geom->bwidth_uv -
                                               1 + j * recon_ptr->stride_cb];
                context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                    .neigh_left_recon_16bit[2][j] =
                    ((uint16_t *)
                         recon_ptr->buffer_cr)[rec_cr_offset + context_ptr->blk_geom->bwidth_uv -
                                               1 + j * recon_ptr->stride_cr];
            }
        }
#if OPT_LOSSLESS_1
    }
#endif
#if NO_ENCDEC
    //copy recon
    uint32_t txb_origin_index = context_ptr->blk_geom->origin_x +
        (context_ptr->blk_geom->origin_y * 128);
    uint32_t bwidth  = context_ptr->blk_geom->bwidth;
    uint32_t bheight = context_ptr->blk_geom->bheight;

    if (!context_ptr->hbd_mode_decision) {
        uint8_t *src_ptr = &(((uint8_t *)candidate_buffer->recon_ptr->buffer_y)[txb_origin_index]);
        uint8_t *dst_ptr = &(((uint8_t *)context_ptr->blk_ptr->recon_tmp->buffer_y)[0]);

        for (uint32_t j = 0; j < bheight; j++)
            svt_memcpy(dst_ptr + j * 128, src_ptr + j * 128, bwidth * sizeof(uint8_t));

        if (context_ptr->blk_geom->has_uv) {
            uint32_t txb_origin_index = ((((context_ptr->blk_geom->origin_x >> 3) << 3) +
                                          ((context_ptr->blk_geom->origin_y >> 3) << 3) *
                                              candidate_buffer->recon_ptr->stride_cb) >>
                                         1);
            bwidth                    = context_ptr->blk_geom->bwidth_uv;
            bheight                   = context_ptr->blk_geom->bheight_uv;

            // Cb
            src_ptr = &(((uint8_t *)candidate_buffer->recon_ptr->buffer_cb)[txb_origin_index]);
            dst_ptr = &(((uint8_t *)context_ptr->blk_ptr->recon_tmp->buffer_cb)[0]);

            for (uint32_t j = 0; j < bheight; j++)
                svt_memcpy(dst_ptr + j * 64, src_ptr + j * 64, bwidth * sizeof(uint8_t));

            // Cr
            src_ptr = &(((uint8_t *)candidate_buffer->recon_ptr->buffer_cr)[txb_origin_index]);
            dst_ptr = &(((uint8_t *)context_ptr->blk_ptr->recon_tmp->buffer_cr)[0]);

            for (uint32_t j = 0; j < bheight; j++)
                svt_memcpy(dst_ptr + j * 64, src_ptr + j * 64, bwidth * sizeof(uint8_t));
        }
    } else {
        uint16_t *src_ptr = ((uint16_t *)candidate_buffer->recon_ptr->buffer_y) + txb_origin_index;
        uint16_t *dst_ptr = (uint16_t *)context_ptr->blk_ptr->recon_tmp->buffer_y;
        for (uint32_t j = 0; j < bheight; j++)
            svt_memcpy(dst_ptr + j * 128, src_ptr + j * 128, bwidth * sizeof(uint16_t));

        if (context_ptr->blk_geom->has_uv) {
            txb_origin_index = ((((context_ptr->blk_geom->origin_x >> 3) << 3) +
                                 ((context_ptr->blk_geom->origin_y >> 3) << 3) *
                                     candidate_buffer->recon_ptr->stride_cb) >>
                                1);
            bwidth           = context_ptr->blk_geom->bwidth_uv;
            bheight          = context_ptr->blk_geom->bheight_uv;

            // Cb
            src_ptr = ((uint16_t *)candidate_buffer->recon_ptr->buffer_cb) + txb_origin_index;
            dst_ptr = (uint16_t *)context_ptr->blk_ptr->recon_tmp->buffer_cb;
            for (uint32_t j = 0; j < bheight; j++)
                svt_memcpy(dst_ptr + j * 64, src_ptr + j * 64, bwidth * sizeof(uint16_t));

            // Cr
            src_ptr = ((uint16_t *)candidate_buffer->recon_ptr->buffer_cr) + txb_origin_index;
            dst_ptr = (uint16_t *)context_ptr->blk_ptr->recon_tmp->buffer_cr;
            for (uint32_t j = 0; j < bheight; j++)
                svt_memcpy(dst_ptr + j * 64, src_ptr + j * 64, bwidth * sizeof(uint16_t));
        }
    }
#endif
    context_ptr->avail_blk_flag[blk_ptr->mds_idx] = EB_TRUE;
}

#if FTR_NSQ_RED_USING_RECON
uint8_t update_skip_nsq_based_on_sq_recon_dist(ModeDecisionContext *context_ptr) {
    uint8_t  skip_nsq               = 0;
    uint32_t max_part0_to_part1_dev = context_ptr->max_part0_to_part1_dev;

    // return immediately if SQ, or NSQ but Parent not available, or max_part0_to_part1_dev is set to infinity
    if (context_ptr->blk_geom->shape == PART_N ||
        context_ptr->avail_blk_flag[context_ptr->blk_geom->sqi_mds] == EB_FALSE ||
        max_part0_to_part1_dev == 0)
        return skip_nsq;

#if FTR_MODULATE_SRC_REC_TH
    // Derive the distortion/cost ratio
    uint32_t full_lambda = context_ptr->hbd_mode_decision
        ? context_ptr->full_lambda_md[EB_10_BIT_MD]
        : context_ptr->full_lambda_md[EB_8_BIT_MD];
    uint64_t dist        = RDCOST(
        full_lambda,
        0,
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds].full_distortion);
    uint64_t dist_cost_ratio = (dist * 100) /
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds].cost;
    uint64_t min_ratio    = 0;
    uint64_t max_ratio    = 100;
    uint64_t modulated_th = (100 * (dist_cost_ratio - min_ratio)) / (max_ratio - min_ratio);

    // increase TH by 25 % when Parent is New (go conservative)
    if (context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == NEWMV ||
        context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == NEW_NEWMV)
        max_part0_to_part1_dev = max_part0_to_part1_dev - ((max_part0_to_part1_dev * 25) / 100);
    else
        // decrease TH by 25 % when Parent is Nearest-Near (go agressive)
        if (context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == NEARESTMV ||
            context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode ==
                NEAREST_NEARESTMV ||
            context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == NEARMV ||
            context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == NEAR_NEARMV)
        max_part0_to_part1_dev = max_part0_to_part1_dev + ((max_part0_to_part1_dev * 25) / 100);
#endif

    if (context_ptr->blk_geom->shape == PART_H || context_ptr->blk_geom->shape == PART_HA ||
        context_ptr->blk_geom->shape == PART_HB || context_ptr->blk_geom->shape == PART_H4) {
#if FTR_MODULATE_SRC_REC_TH
        // multiply the TH by 4 when Parent is D45 or D135 (diagonal) or when Parent is D67 / V / D113 (H_path)
        if (context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == V_PRED ||
            context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == D67_PRED ||
            context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == D113_PRED ||
            context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == D45_PRED ||
            context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == D135_PRED)
            max_part0_to_part1_dev = max_part0_to_part1_dev << 2;
#endif
        uint64_t dist_h0 = MAX(1,
                               context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                       .rec_dist_per_quadrant[0] +
                                   context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                       .rec_dist_per_quadrant[1]);

        uint64_t dist_h1 = MAX(1,
                               context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                       .rec_dist_per_quadrant[2] +
                                   context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                       .rec_dist_per_quadrant[3]);

        uint32_t dev = (uint32_t)((ABS((int32_t)(dist_h0 - dist_h1)) * 100) /
                                  MIN(dist_h0, dist_h1));
#if FTR_MODULATE_SRC_REC_TH
        // TH = TH + TH * Min(dev_0,dev_1); dev_0 is q0 - to - q1 deviation, and dev_1 is q2 - to - q3 deviation
        uint64_t dist_q0 = MAX(1,
                               context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                   .rec_dist_per_quadrant[0]);
        uint64_t dist_q1 = MAX(1,
                               context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                   .rec_dist_per_quadrant[1]);
        uint64_t dist_q2 = MAX(1,
                               context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                   .rec_dist_per_quadrant[2]);
        uint64_t dist_q3 = MAX(1,
                               context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                   .rec_dist_per_quadrant[3]);

        uint32_t quad_dev_t    = (uint32_t)((ABS((int32_t)(dist_q0 - dist_q1)) * 100) /
                                         MIN(dist_q0, dist_q1));
        uint32_t quad_dev_b    = (uint32_t)((ABS((int32_t)(dist_q2 - dist_q3)) * 100) /
                                         MIN(dist_q2, dist_q3));
        max_part0_to_part1_dev = max_part0_to_part1_dev +
            ((max_part0_to_part1_dev * MIN(quad_dev_t, quad_dev_b)) / 100);

        max_part0_to_part1_dev = (uint32_t)((dist_cost_ratio <= min_ratio) ? 0
                                                : (dist_cost_ratio <= max_ratio)
                                                ? (max_part0_to_part1_dev * modulated_th) / 100
                                                : dist_cost_ratio);
#endif
        if (dev < max_part0_to_part1_dev)
            return EB_TRUE;
    }

    if (context_ptr->blk_geom->shape == PART_V || context_ptr->blk_geom->shape == PART_VA ||
        context_ptr->blk_geom->shape == PART_VB || context_ptr->blk_geom->shape == PART_V4) {
#if FTR_MODULATE_SRC_REC_TH
        // multiply the TH by 4 when Parent is D45 or D135 (diagonal) or when Parent is D157 / H / D203 (V_path)
        if (context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == H_PRED ||
            context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == D157_PRED ||
            context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == D203_PRED ||
            context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == D45_PRED ||
            context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds].pred_mode == D135_PRED)
            max_part0_to_part1_dev = max_part0_to_part1_dev << 2;
#endif
        uint64_t dist_v0 = MAX(1,
                               context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                       .rec_dist_per_quadrant[0] +
                                   context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                       .rec_dist_per_quadrant[2]);

        uint64_t dist_v1 = MAX(1,
                               context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                       .rec_dist_per_quadrant[1] +
                                   context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                       .rec_dist_per_quadrant[3]);

        uint32_t dev = (uint32_t)((ABS((int32_t)(dist_v0 - dist_v1)) * 100) /
                                  MIN(dist_v0, dist_v1));

#if FTR_MODULATE_SRC_REC_TH
        // TH = TH + TH * Min(dev_0,dev_1); dev_0 is q0-to-q2 deviation, and dev_1 is q1-to-q3 deviation
        uint64_t dist_q0 = MAX(1,
                               context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                   .rec_dist_per_quadrant[0]);
        uint64_t dist_q1 = MAX(1,
                               context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                   .rec_dist_per_quadrant[1]);
        uint64_t dist_q2 = MAX(1,
                               context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                   .rec_dist_per_quadrant[2]);
        uint64_t dist_q3 = MAX(1,
                               context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                                   .rec_dist_per_quadrant[3]);

        uint32_t quad_dev_l = (uint32_t)((ABS((int32_t)(dist_q0 - dist_q2)) * 100) /
                                         MIN(dist_q0, dist_q2));
        uint32_t quad_dev_r = (uint32_t)((ABS((int32_t)(dist_q1 - dist_q3)) * 100) /
                                         MIN(dist_q1, dist_q3));

        max_part0_to_part1_dev = max_part0_to_part1_dev +
            ((max_part0_to_part1_dev * MIN(quad_dev_l, quad_dev_r)) / 100);

        max_part0_to_part1_dev = (uint32_t)((dist_cost_ratio <= min_ratio) ? 0
                                                : (dist_cost_ratio <= max_ratio)
                                                ? (max_part0_to_part1_dev * modulated_th) / 100
                                                : dist_cost_ratio);
#endif
        if (dev < max_part0_to_part1_dev)
            return EB_TRUE;
    }

    return skip_nsq;
}
#endif

/*
 * Determine if the evaluation of nsq blocks (HA, HB, VA, VB, H4, V4) can be skipped
 * based on the relative cost of the SQ, H, and V blocks.  The scaling factor sq_weight
 * determines how likely it is to skip blocks, and is a function of the qp, block shape,
 * prediction mode, block coeffs, and encode mode.
 *
 * skip HA, HB and H4 if (valid SQ and H) and (H_COST > (SQ_WEIGHT * SQ_COST) / 100)
 * skip VA, VB and V4 if (valid SQ and V) and (V_COST > (SQ_WEIGHT * SQ_COST) / 100)
 *
 * Returns TRUE if the blocks should be skipped; FALSE otherwise.
 */
uint8_t update_skip_nsq_shapes(ModeDecisionContext *context_ptr) {
    uint8_t  skip_nsq  = 0;
    uint32_t sq_weight = context_ptr->sq_weight;

    // return immediately if the skip nsq threshold is infinite
    if (sq_weight == (uint32_t)~0)
        return skip_nsq;
    // use a conservative threshold for H4, V4 blocks
    if (context_ptr->blk_geom->shape == PART_H4 || context_ptr->blk_geom->shape == PART_V4)
        sq_weight += CONSERVATIVE_OFFSET_0;

    uint32_t     sqi           = context_ptr->blk_geom->sqi_mds;
    MdBlkStruct *local_cu_unit = context_ptr->md_local_blk_unit;

    if (context_ptr->blk_geom->shape == PART_HA || context_ptr->blk_geom->shape == PART_HB ||
        context_ptr->blk_geom->shape == PART_H4) {
        if (context_ptr->avail_blk_flag[context_ptr->blk_geom->sqi_mds] &&
            context_ptr->avail_blk_flag[context_ptr->blk_geom->sqi_mds + 1] &&
            context_ptr->avail_blk_flag[context_ptr->blk_geom->sqi_mds + 2]) {
            // Use aggressive thresholds for blocks without coeffs
            if (context_ptr->blk_geom->shape == PART_HA) {
                if (!context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds + 1]
                         .block_has_coeff)
                    sq_weight += AGGRESSIVE_OFFSET_1;
            }
            if (context_ptr->blk_geom->shape == PART_HB) {
                if (!context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds + 2]
                         .block_has_coeff)
                    sq_weight += AGGRESSIVE_OFFSET_1;
            }

            // compute the cost of the SQ block and H block
            uint64_t sq_cost =
                context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds].default_cost;
            uint64_t h_cost =
                context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds + 1].default_cost +
                context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds + 2].default_cost;

            // Determine if nsq shapes can be skipped based on the relative cost of SQ and H blocks
            skip_nsq = (h_cost > ((sq_cost * sq_weight) / 100));
            // If not skipping, perform a check on the relative H/V costs
            if (!skip_nsq) {
                if (context_ptr->avail_blk_flag[sqi + 3] && context_ptr->avail_blk_flag[sqi + 4]) {
                    //compute the cost of V partition
                    uint64_t v_cost = local_cu_unit[sqi + 3].default_cost +
                        local_cu_unit[sqi + 4].default_cost;
                    uint32_t offset   = 10;
                    uint32_t v_weight = 100 + offset;

                    //if the cost of H partition is bigger than the V partition by a certain percentage, skip HA/HB
                    //use 10% to skip HA/HB, use 5% to skip H4, also for very low QP be more aggressive to skip
                    skip_nsq = (h_cost > ((v_cost * v_weight) / 100));
                }
            }
        }
    }
    if (context_ptr->blk_geom->shape == PART_VA || context_ptr->blk_geom->shape == PART_VB ||
        context_ptr->blk_geom->shape == PART_V4) {
        if (context_ptr->avail_blk_flag[context_ptr->blk_geom->sqi_mds] &&
            context_ptr->avail_blk_flag[context_ptr->blk_geom->sqi_mds + 3] &&
            context_ptr->avail_blk_flag[context_ptr->blk_geom->sqi_mds + 4]) {
            // Use aggressive thresholds for blocks without coeffs
            if (context_ptr->blk_geom->shape == PART_VA) {
                if (!context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds + 3]
                         .block_has_coeff)
                    sq_weight += AGGRESSIVE_OFFSET_1;
            }
            if (context_ptr->blk_geom->shape == PART_VB) {
                if (!context_ptr->md_blk_arr_nsq[context_ptr->blk_geom->sqi_mds + 4]
                         .block_has_coeff)
                    sq_weight += AGGRESSIVE_OFFSET_1;
            }

            // compute the cost of the SQ block and V block
            uint64_t sq_cost =
                context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds].default_cost;
            uint64_t v_cost =
                context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds + 3].default_cost +
                context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds + 4].default_cost;

            // Determine if nsq shapes can be skipped based on the relative cost of SQ and V blocks
            skip_nsq = (v_cost > ((sq_cost * sq_weight) / 100));
            // If not skipping, perform a check on the relative H/V costs
            if (!skip_nsq) {
                if (context_ptr->avail_blk_flag[sqi + 1] && context_ptr->avail_blk_flag[sqi + 2]) {
                    uint64_t h_cost = local_cu_unit[sqi + 1].default_cost +
                        local_cu_unit[sqi + 2].default_cost;
                    uint32_t offset = 10;

                    uint32_t h_weight = 100 + offset;
                    skip_nsq          = (v_cost > ((h_cost * h_weight) / 100));
                }
            }
        }
    }

    return skip_nsq;
}
#if FTR_NEW_CYCLES_ALLOC
void md_pme_search_controls(ModeDecisionContext *ctx, uint8_t md_pme_level);
void set_inter_intra_ctrls(ModeDecisionContext *ctx, uint8_t inter_intra_level);
#if OPT_INIT
void set_dist_based_ref_pruning_controls(ModeDecisionContext *mdctxt, uint8_t dist_based_ref_pruning_level);
#else
void md_pme_search_controls(ModeDecisionContext *ctx, uint8_t md_pme_level);
#endif
void set_txt_controls(ModeDecisionContext *ctx, uint8_t txt_level);

// Set the levels used by features which apply aggressive settings for certain blocks (e.g. NSQ stats)
// Return 1 to skip, else 0
//
// Level 0 - skip
// Level 1-4 - adjust certain settings
EbBool update_md_settings(ModeDecisionContext *ctx, uint8_t level) {
    // Level 0 is skip
    if (level == 0)
        return 1;

    if (level >= 1) {
        // Don't make NICs more conservative
        ctx->nic_ctrls.stage1_scaling_num = MIN(ctx->nic_ctrls.stage1_scaling_num, 5);
        ctx->nic_ctrls.stage2_scaling_num = MIN(ctx->nic_ctrls.stage2_scaling_num, 3);
        ctx->nic_ctrls.stage3_scaling_num = MIN(ctx->nic_ctrls.stage3_scaling_num, 3);
        ctx->md_tx_size_search_mode       = 0;
    }
    if (level >= 2) {
        set_compound_to_inject(ctx,
                               ctx->inter_comp_ctrls.allowed_comp_types,
                               1 /*AVG*/,
                               0 /*DIST*/,
                               0 /*DIFF*/,
                               0 /*WEDGE*/);
        ctx->md_inter_intra_level = 0;
        set_inter_intra_ctrls(ctx, ctx->md_inter_intra_level);
        ctx->md_pme_level = 3;
        md_pme_search_controls(ctx, ctx->md_pme_level);
    }
    if (level >= 3) {
#if TUNE_NEW_PRESETS_MR_M8
        ctx->dist_based_ref_pruning = 6;
#else
        ctx->dist_based_ref_pruning = 5;
#endif
        set_dist_based_ref_pruning_controls(ctx, ctx->dist_based_ref_pruning);
        ctx->nic_ctrls.stage1_scaling_num = MIN(ctx->nic_ctrls.stage1_scaling_num, 2);
        ctx->nic_ctrls.stage2_scaling_num = MIN(ctx->nic_ctrls.stage2_scaling_num, 1);
        ctx->nic_ctrls.stage3_scaling_num = MIN(ctx->nic_ctrls.stage3_scaling_num, 1);
    }
    if (level >= 4) {
        set_txt_controls(ctx, 5);
        ctx->chroma_level = CHROMA_MODE_1;
    }
    return 0;
}

// Update MD settings or skip NSQ block based on the coeff-area of the parent SQ block
// Returns 1 to skip the NSQ block; 0 otherwise
uint8_t update_md_settings_based_on_sq_coeff_area(ModeDecisionContext *ctx) {
    uint8_t                                     skip_nsq = 0;
    ParentSqCoeffAreaBasedCyclesReductionCtrls *cycles_red_ctrls =
        &ctx->parent_sq_coeff_area_based_cycles_reduction_ctrls;
    if (cycles_red_ctrls->enabled) {
        if (ctx->blk_geom->shape != PART_N) {
            if (ctx->avail_blk_flag[ctx->blk_geom->sqi_mds]) {
                uint32_t count_non_zero_coeffs =
                    ctx->md_local_blk_unit[ctx->blk_geom->sqi_mds].count_non_zero_coeffs;
                uint32_t total_samples = (ctx->blk_geom->sq_size * ctx->blk_geom->sq_size);

                // High frequency band actions
                if (count_non_zero_coeffs >=
                    ((total_samples * cycles_red_ctrls->high_freq_band1_th) / 100))
                    skip_nsq = update_md_settings(ctx, cycles_red_ctrls->high_freq_band1_level);
                else if (count_non_zero_coeffs >=
                         ((total_samples * cycles_red_ctrls->high_freq_band2_th) / 100))
                    skip_nsq = update_md_settings(ctx, cycles_red_ctrls->high_freq_band2_level);
#if TUNE_LOWER_PRESETS
                else if (count_non_zero_coeffs >=
                         ((total_samples * cycles_red_ctrls->high_freq_band3_th) / 100))
                    skip_nsq = update_md_settings(ctx, cycles_red_ctrls->high_freq_band3_level);
#endif
                // Low frequency band actions
                else if (cycles_red_ctrls->enable_zero_coeff_action && count_non_zero_coeffs == 0) {
                    skip_nsq = update_md_settings(ctx, cycles_red_ctrls->zero_coeff_action);
                    set_txt_controls(ctx, 0);
                } else if (cycles_red_ctrls->enable_one_coeff_action && count_non_zero_coeffs == 1)
                    skip_nsq = update_md_settings(ctx, cycles_red_ctrls->one_coeff_action);
                else if (count_non_zero_coeffs <
                         ((total_samples * cycles_red_ctrls->low_freq_band1_th) / 100))
                    skip_nsq = update_md_settings(ctx, cycles_red_ctrls->low_freq_band1_level);
                else if (count_non_zero_coeffs <
                         ((total_samples * cycles_red_ctrls->low_freq_band2_th) / 100))
                    skip_nsq = update_md_settings(ctx, cycles_red_ctrls->low_freq_band2_level);
            }
        }
    }
    return skip_nsq;
}
#else
/***********************************
get the number of total block in a
branch
***********************************/
uint32_t get_number_of_blocks(uint32_t block_idx) {
    const BlockGeom *blk_geom = get_blk_geom_mds(block_idx);
    uint32_t tot_d1_blocks = blk_geom->sq_size == 128 ? 17
        : blk_geom->sq_size > 8 ? 25
        : blk_geom->sq_size == 8 ? 5
                                 : 1;
    return tot_d1_blocks;
}
uint8_t get_allowed_block(ModeDecisionContext *context_ptr) {
    uint8_t skip_nsq = 0;
    uint8_t sq_size_idx = 7 - (uint8_t)svt_log2f((uint8_t)context_ptr->blk_geom->sq_size);
    if (context_ptr->coeff_area_based_bypass_nsq_th) {
        if (context_ptr->blk_geom->shape != PART_N) {
            if (context_ptr->avail_blk_flag[context_ptr->blk_geom->sqi_mds]) {
                uint32_t count_non_zero_coeffs =
                    context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                        .count_non_zero_coeffs;
                uint32_t total_samples = (context_ptr->blk_geom->bwidth *
                                          context_ptr->blk_geom->bheight);
                uint8_t band_idx = 0;
                uint64_t band_width = (sq_size_idx == 0) ? 100 : (sq_size_idx == 1) ? 50 : 20;
                if (count_non_zero_coeffs >= ((total_samples * 18) / band_width)) {
                    band_idx = 9;
                } else if (count_non_zero_coeffs >= ((total_samples * 16) / band_width)) {
                    band_idx = 8;
                } else if (count_non_zero_coeffs >= ((total_samples * 14) / band_width)) {
                    band_idx = 7;
                } else if (count_non_zero_coeffs >= ((total_samples * 12) / band_width)) {
                    band_idx = 6;
                } else if (count_non_zero_coeffs >= ((total_samples * 10) / band_width)) {
                    band_idx = 5;
                } else if (count_non_zero_coeffs >= ((total_samples * 8) / band_width)) {
                    band_idx = 4;
                } else if (count_non_zero_coeffs >= ((total_samples * 6) / band_width)) {
                    band_idx = 3;
                } else if (count_non_zero_coeffs >= ((total_samples * 4) / band_width)) {
                    band_idx = 2;
                } else if (count_non_zero_coeffs >= ((total_samples * 2) / band_width)) {
                    band_idx = 1;
                } else {
                    band_idx = 0;
                }

                if (sq_size_idx == 0)
                    band_idx = band_idx == 0 ? 0 : band_idx <= 2 ? 1 : 2;
                else if (sq_size_idx == 1)
                    band_idx = band_idx == 0 ? 0 : band_idx <= 3 ? 1 : 2;
                else
                    band_idx = band_idx == 0 ? 0 : band_idx <= 8 ? 1 : 2;
                uint8_t sse_gradian_band =
                    context_ptr->avail_blk_flag[context_ptr->blk_geom->sqi_mds]
                    ? context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                          .sse_gradian_band[context_ptr->blk_geom->shape]
                    : 1;
                if (context_ptr->coeff_area_based_bypass_nsq_th) {
                    uint64_t nsq_prob_cycles_allocation =
                        block_prob_tab[sq_size_idx][context_ptr->blk_geom->shape][band_idx]
                                      [sse_gradian_band];
                    if ((nsq_prob_cycles_allocation < context_ptr->coeff_area_based_bypass_nsq_th))
                        skip_nsq = 1;
                }
            }
        }
    }
    return skip_nsq;
}

void md_pme_search_controls(ModeDecisionContext *mdctxt, uint8_t md_pme_level);
void set_inter_intra_ctrls(ModeDecisionContext *mdctxt, uint8_t inter_intra_level);

// Set the levels used by features which apply aggressive settings for certain blocks (e.g. NSQ stats)
void update_md_settings(ModeDecisionContext *context_ptr, uint8_t level) {
    // Don't make NICs more conservative
    context_ptr->nic_ctrls.stage1_scaling_num = MIN(context_ptr->nic_ctrls.stage1_scaling_num, 5);
    context_ptr->nic_ctrls.stage2_scaling_num = MIN(context_ptr->nic_ctrls.stage2_scaling_num, 3);
    context_ptr->nic_ctrls.stage3_scaling_num = MIN(context_ptr->nic_ctrls.stage3_scaling_num, 3);
    context_ptr->md_tx_size_search_mode = 0;

    if (level >= 1) {
        // Allow AVG compound only
        set_compound_to_inject(context_ptr,
                               context_ptr->inter_comp_ctrls.allowed_comp_types,
                               1 /*AVG*/,
                               0 /*DIST*/,
                               0 /*DIFF*/,
                               0 /*WEDGE*/);
        context_ptr->md_inter_intra_level = 0;
        set_inter_intra_ctrls(context_ptr, context_ptr->md_inter_intra_level);
        context_ptr->md_pme_level = 3;
        md_pme_search_controls(context_ptr, context_ptr->md_pme_level);
    }
}
#endif
#if !CLN_NSQ_AND_STATS
void update_md_settings_based_on_stats(ModeDecisionContext *context_ptr,
                                       int8_t               pred_depth_refinement) {
    // Use more aggressive (faster, but less accurate) settigns for unlikely paritions (incl. SQ)
    AMdCycleRControls *adaptive_md_cycles_red_ctrls = &context_ptr->admd_cycles_red_ctrls;
    if (adaptive_md_cycles_red_ctrls->enabled) {
        pred_depth_refinement = MIN(pred_depth_refinement, 1);
        pred_depth_refinement = MAX(pred_depth_refinement, -1);
        pred_depth_refinement += 2;
        if (context_ptr->ad_md_prob[pred_depth_refinement][context_ptr->blk_geom->shape] <
            adaptive_md_cycles_red_ctrls->switch_level_th) {
            update_md_settings(context_ptr, adaptive_md_cycles_red_ctrls->non_skip_level);
        }
    }
}
#endif
#if !FTR_NEW_CYCLES_ALLOC
uint8_t update_md_settings_based_on_sq_coeff(ModeDecisionContext *context_ptr) {
    // If SQ block has zero coeffs, use more aggressive settings (or skip) for NSQ blocks
    uint8_t          zero_sq_coeff_skip_action = 0;
    uint8_t          sq_index                  = svt_log2f(context_ptr->blk_geom->sq_size) - 2;
    CoeffBSwMdCtrls *coeffb_sw_md_ctrls        = &context_ptr->cb_sw_md_ctrls;
    if (coeffb_sw_md_ctrls->enabled) {
        EbBool switch_md_mode_based_on_sq_coeff = EB_FALSE;
        if (context_ptr->avail_blk_flag[context_ptr->blk_geom->sqi_mds])
            switch_md_mode_based_on_sq_coeff = context_ptr->blk_geom->shape == PART_N ||
                    context_ptr->parent_sq_has_coeff[sq_index] != 0
                ? EB_FALSE
                : EB_TRUE;

        if (switch_md_mode_based_on_sq_coeff) {
            if (coeffb_sw_md_ctrls->skip_block) {
                zero_sq_coeff_skip_action = 1;
            } else {
                update_md_settings(context_ptr, coeffb_sw_md_ctrls->non_skip_level);
                // Turn off TXT search because if have zero coeffs tx_type must be DCT_DCT, and if SQ has zero coeffs,
                // NSQ blocks are likely to also have zero coeffs
                context_ptr->md_staging_txt_level = 0;
            }
        }
    }
    return zero_sq_coeff_skip_action;
}
#endif
#if RFCTR_MD_BLOCK_LOOP
/*
 * Pad high bit depth pictures.
 *
 * Returns pointer to padded data.
 */
EbPictureBufferDesc *pad_hbd_pictures(SequenceControlSet *scs, PictureControlSet *pcs,
                                      ModeDecisionContext *ctx, EbPictureBufferDesc *in_pic,
                                      uint16_t sb_org_x, uint16_t sb_org_y) {
    const uint32_t input_luma_offset = ((sb_org_y + in_pic->origin_y) * in_pic->stride_y) +
        (sb_org_x + in_pic->origin_x);
    const uint32_t input_bit_inc_luma_offset = ((sb_org_y + in_pic->origin_y) *
                                                in_pic->stride_bit_inc_y) +
        (sb_org_x + in_pic->origin_x);
    const uint32_t input_cb_offset = (((sb_org_y + in_pic->origin_y) >> 1) * in_pic->stride_cb) +
        ((sb_org_x + in_pic->origin_x) >> 1);
    const uint32_t input_bit_inc_cb_offset = (((sb_org_y + in_pic->origin_y) >> 1) *
                                              in_pic->stride_bit_inc_cb) +
        ((sb_org_x + in_pic->origin_x) >> 1);
    const uint32_t input_cr_offset = (((sb_org_y + in_pic->origin_y) >> 1) * in_pic->stride_cr) +
        ((sb_org_x + in_pic->origin_x) >> 1);
    const uint32_t input_bit_inc_cr_offset = (((sb_org_y + in_pic->origin_y) >> 1) *
                                              in_pic->stride_bit_inc_cr) +
        ((sb_org_x + in_pic->origin_x) >> 1);

    uint32_t sb_width  = MIN(scs->sb_size_pix, pcs->parent_pcs_ptr->aligned_width - sb_org_x);
    uint32_t sb_height = MIN(scs->sb_size_pix, pcs->parent_pcs_ptr->aligned_height - sb_org_y);

    pack2d_src(in_pic->buffer_y + input_luma_offset,
               in_pic->stride_y,
               in_pic->buffer_bit_inc_y + input_bit_inc_luma_offset,
               in_pic->stride_bit_inc_y,
               (uint16_t *)ctx->input_sample16bit_buffer->buffer_y,
               ctx->input_sample16bit_buffer->stride_y,
               sb_width,
               sb_height);

    pack2d_src(in_pic->buffer_cb + input_cb_offset,
               in_pic->stride_cb,
               in_pic->buffer_bit_inc_cb + input_bit_inc_cb_offset,
               in_pic->stride_bit_inc_cb,
               (uint16_t *)ctx->input_sample16bit_buffer->buffer_cb,
               ctx->input_sample16bit_buffer->stride_cb,
               sb_width >> 1,
               sb_height >> 1);

    pack2d_src(in_pic->buffer_cr + input_cr_offset,
               in_pic->stride_cr,
               in_pic->buffer_bit_inc_cr + input_bit_inc_cr_offset,
               in_pic->stride_bit_inc_cr,
               (uint16_t *)ctx->input_sample16bit_buffer->buffer_cr,
               ctx->input_sample16bit_buffer->stride_cr,
               sb_width >> 1,
               sb_height >> 1);
    // PAD the packed source in incomplete sb up to max SB size
    pad_input_picture_16bit((uint16_t *)ctx->input_sample16bit_buffer->buffer_y,
                            ctx->input_sample16bit_buffer->stride_y,
                            sb_width,
                            sb_height,
                            scs->sb_size_pix - sb_width,
                            scs->sb_size_pix - sb_height);

    pad_input_picture_16bit((uint16_t *)ctx->input_sample16bit_buffer->buffer_cb,
                            ctx->input_sample16bit_buffer->stride_cb,
                            sb_width >> 1,
                            sb_height >> 1,
                            (scs->sb_size_pix - sb_width) >> 1,
                            (scs->sb_size_pix - sb_height) >> 1);

    pad_input_picture_16bit((uint16_t *)ctx->input_sample16bit_buffer->buffer_cr,
                            ctx->input_sample16bit_buffer->stride_cr,
                            sb_width >> 1,
                            sb_height >> 1,
                            (scs->sb_size_pix - sb_width) >> 1,
                            (scs->sb_size_pix - sb_height) >> 1);
    store16bit_input_src(
        ctx->input_sample16bit_buffer, pcs, sb_org_x, sb_org_y, scs->sb_size_pix, scs->sb_size_pix);

    return !use_output_stat(scs) ? pcs->input_frame16bit : in_pic;
}

/*
 * Update the neighbour arrays before starting block processing.
 */
void update_neighbour_arrays(PictureControlSet *pcs, ModeDecisionContext *ctx) {
    const uint16_t tile_idx = ctx->tile_index;

    // Mode Decision Neighbor Arrays
    ctx->intra_luma_mode_neighbor_array =
        pcs->md_intra_luma_mode_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
#if !CLN_MDC_CTX // start
    ctx->intra_chroma_mode_neighbor_array =
        pcs->md_intra_chroma_mode_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    ctx->mv_neighbor_array = pcs->md_mv_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
#endif
    ctx->skip_flag_neighbor_array =
        pcs->md_skip_flag_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    ctx->mode_type_neighbor_array =
        pcs->md_mode_type_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    ctx->leaf_partition_neighbor_array =
        pcs->mdleaf_partition_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];

    if (!ctx->hbd_mode_decision) {
        ctx->luma_recon_neighbor_array =
            pcs->md_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
        ctx->cb_recon_neighbor_array =
            pcs->md_cb_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
        ctx->cr_recon_neighbor_array =
            pcs->md_cr_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    } else {
        ctx->luma_recon_neighbor_array16bit =
            pcs->md_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
        ctx->cb_recon_neighbor_array16bit =
            pcs->md_cb_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
        ctx->cr_recon_neighbor_array16bit =
            pcs->md_cr_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    }
    ctx->luma_dc_sign_level_coeff_neighbor_array =
        pcs->md_luma_dc_sign_level_coeff_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    ctx->cb_dc_sign_level_coeff_neighbor_array =
        pcs->md_cb_dc_sign_level_coeff_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    ctx->cr_dc_sign_level_coeff_neighbor_array =
        pcs->md_cr_dc_sign_level_coeff_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    ctx->txfm_context_array = pcs->md_txfm_context_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
#if !CLN_MDC_CTX
    ctx->inter_pred_dir_neighbor_array =
        pcs->md_inter_pred_dir_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
#endif
    ctx->ref_frame_type_neighbor_array =
        pcs->md_ref_frame_type_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    ctx->interpolation_type_neighbor_array =
        pcs->md_interpolation_type_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
}

/*
 * Initialize data needed for processing each block.  Update neighbour array if the block
 * is the first d1 block.  Called before process each block.
 */
void init_block_data(PictureControlSet *pcs, ModeDecisionContext *ctx,
                     const EbMdcLeafData *const leaf_data_ptr,
#if OPT6_DEPTH_REFINEMENT
                     const uint8_t blk_split_flag,
#endif
#if !OPT_INIT_XD
                     SuperBlock *sb_ptr,
#endif
                     uint16_t sb_org_x, uint16_t sb_org_y, uint32_t blk_idx_mds,
                     uint8_t first_d1_blk) {

    const BlockGeom *blk_geom = ctx->blk_geom;
    BlkStruct *      blk_ptr  = ctx->blk_ptr;
#if !OPT_INIT
    ctx->cu_size_log2 = blk_geom->bwidth_log2;
#endif
    ctx->blk_origin_x = sb_org_x + blk_geom->origin_x;
    ctx->blk_origin_y = sb_org_y + blk_geom->origin_y;
#if !OPT_INIT
    ctx->sb_sz = BLOCK_SIZE_64;
#endif
    ctx->round_origin_x = ((ctx->blk_origin_x >> 3) << 3);
    ctx->round_origin_y = ((ctx->blk_origin_y >> 3) << 3);
    ctx->sb_origin_x    = sb_org_x;
    ctx->sb_origin_y    = sb_org_y;
#if OPT_BUILD_CAND_BLK_2
    ctx->tested_blk_flag[blk_idx_mds] = EB_TRUE;
#else
    ctx->md_local_blk_unit[blk_idx_mds].tested_blk_flag = EB_TRUE;
#endif
    ctx->md_ep_pipe_sb[blk_idx_mds].merge_cost = 0;
    ctx->md_ep_pipe_sb[blk_idx_mds].skip_cost = 0;
#if !OPT_INIT
    blk_ptr->av1xd->sb_type = blk_geom->bsize;
#endif
    blk_ptr->mds_idx                           = blk_idx_mds;
#if OPT6_DEPTH_REFINEMENT
    ctx->md_blk_arr_nsq[blk_idx_mds].mdc_split_flag   = blk_split_flag;
#if !OPT_INIT
    ctx->md_blk_arr_nsq[blk_geom->sqi_mds].split_flag = blk_split_flag;
#endif
    blk_ptr->split_flag = blk_split_flag; //mdc indicates smallest or non valid CUs with split flag=
#else
    ctx->md_blk_arr_nsq[blk_idx_mds].mdc_split_flag = (uint16_t)leaf_data_ptr->split_flag;
#if !OPT_REFINEMENT_SIGNALS
    ctx->md_local_blk_unit[blk_idx_mds].pred_depth_refinement =
        leaf_data_ptr->final_pred_depth_refinement;
    ctx->md_local_blk_unit[blk_idx_mds].pred_depth = leaf_data_ptr->final_pred_depth;
#endif
    ctx->md_blk_arr_nsq[blk_geom->sqi_mds].split_flag = (uint16_t)leaf_data_ptr->split_flag;
    blk_ptr->split_flag =
        (uint16_t)
            leaf_data_ptr->split_flag; //mdc indicates smallest or non valid CUs with split flag=
#endif
    blk_ptr->qindex                                 = ctx->qp_index;
#if !OPT_INIT
    ctx->md_local_blk_unit[blk_idx_mds].best_d1_blk = blk_idx_mds;
#endif
#if OPT_BUILD_CAND_BLK_2
    ctx->md_local_blk_unit[blk_idx_mds].left_neighbor_partition  = INVALID_NEIGHBOR_DATA;
    ctx->md_local_blk_unit[blk_idx_mds].above_neighbor_partition = INVALID_NEIGHBOR_DATA;
#endif
#if OPT_MFMV
    ctx->sb64_sq_no4xn_geom = 0;
    if (pcs->parent_pcs_ptr->scs_ptr->static_config.super_block_size == 64 &&
        blk_geom->bwidth == blk_geom->bheight && blk_geom->bsize > BLOCK_8X4)
        ctx->sb64_sq_no4xn_geom = 1;
#endif

    if (leaf_data_ptr->tot_d1_blocks != 1) {
        // We need to get the index of the sq_block for each NSQ branch
        if (first_d1_blk) {
            copy_neighbour_arrays( //save a clean neigh in [1], encode uses [0], reload the clean in [0] after done last ns block in a partition
                pcs,
                ctx,
                0,
                1,
                blk_geom->sqi_mds,
                sb_org_x,
                sb_org_y);
        }
    }
#if !OPT_INIT_XD
    int32_t       mi_row           = ctx->blk_origin_y >> MI_SIZE_LOG2;
    int32_t       mi_col           = ctx->blk_origin_x >> MI_SIZE_LOG2;
    int           mi_stride        = pcs->parent_pcs_ptr->av1_cm->mi_stride;
    const int32_t offset           = mi_row * mi_stride + mi_col;
    blk_ptr->av1xd->mi             = pcs->mi_grid_base + offset;
    ModeInfo *mi_ptr               = *blk_ptr->av1xd->mi;
    blk_ptr->av1xd->up_available   = (mi_row > sb_ptr->tile_info.mi_row_start);
    blk_ptr->av1xd->left_available = (mi_col > sb_ptr->tile_info.mi_col_start);
    if (blk_ptr->av1xd->up_available)
        blk_ptr->av1xd->above_mbmi = &mi_ptr[-mi_stride].mbmi;
    else
        blk_ptr->av1xd->above_mbmi = NULL;
    if (blk_ptr->av1xd->left_available)
        blk_ptr->av1xd->left_mbmi = &mi_ptr[-1].mbmi;
    else
        blk_ptr->av1xd->left_mbmi = NULL;

    // Initialize tx_depth
    blk_ptr->tx_depth = 0;
#endif
}

/*
 * Check cost of current depth to parent depth, and if current cost is larger, signal to exit
 * processing depth early.
 */
void check_curr_to_parent_cost(SequenceControlSet *scs, PictureControlSet *pcs,
                               ModeDecisionContext *ctx, uint32_t sb_addr,
                               uint32_t *next_non_skip_blk_idx_mds, EbBool *md_early_exit_sq,
                               uint8_t d1_blk_count) {
    const BlockGeom *blk_geom = ctx->blk_geom;
    BlkStruct *      blk_ptr  = ctx->blk_ptr;

    if (blk_geom->quadi > 0 && d1_blk_count == 0 && !(*md_early_exit_sq)) {
        uint64_t parent_depth_cost = 0, current_depth_cost = 0;

        // from a given child index, derive the index of the parent
        uint32_t parent_depth_idx_mds =
            (blk_geom->sqi_mds -
             (blk_geom->quadi - 3) *
                 ns_depth_offset[scs->seq_header.sb_size == BLOCK_128X128][blk_geom->depth]) -
            parent_depth_offset[scs->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];

        if ((pcs->slice_type == I_SLICE && parent_depth_idx_mds == 0 &&
             scs->seq_header.sb_size == BLOCK_128X128) ||
            !pcs->parent_pcs_ptr->sb_geom[sb_addr].block_is_allowed[parent_depth_idx_mds])
            parent_depth_cost = MAX_MODE_COST;
        else
            compute_depth_costs_md_skip(
                ctx,
                scs,
                pcs->parent_pcs_ptr,
                parent_depth_idx_mds,
                ns_depth_offset[scs->seq_header.sb_size == BLOCK_128X128][blk_geom->depth],
                &parent_depth_cost,
                &current_depth_cost);

            // compare the cost of the parent to the cost of the already encoded child + an estimated cost for the remaining child @ the current depth
            // if the total child cost is higher than the parent cost then skip the remaining  child @ the current depth
#if TUNE_M10_MD_EXIT
#if OPT_REFACTOR_IN_DEPTH_CTRLS
        uint16_t in_depth_block_skip_weight = ctx->in_depth_block_skip_ctrls.base_weight;
        const BlockGeom *parent_blk_geom = get_blk_geom_mds(parent_depth_idx_mds);

        uint32_t full_lambda = ctx->hbd_mode_decision ?
            ctx->full_lambda_md[EB_10_BIT_MD] :
            ctx->full_lambda_md[EB_8_BIT_MD];

        // child count based modulation
        if (ctx->in_depth_block_skip_ctrls.child_cnt_based_modulation) {
            in_depth_block_skip_weight = (in_depth_block_skip_weight * ctx->in_depth_block_skip_ctrls.cnt_based_weight[ctx->blk_geom->quadi - 1]) / 100;
        }

        // cost band based modulation
        if (ctx->in_depth_block_skip_ctrls.cost_band_based_modulation) {

            uint64_t max_cost = RDCOST(full_lambda, 16, ctx->in_depth_block_skip_ctrls.max_cost_multiplier * parent_blk_geom->bwidth * parent_blk_geom->bheight);

            if (ctx->md_local_blk_unit[parent_depth_idx_mds].default_cost <= max_cost) {
                uint64_t band_size = max_cost / ctx->in_depth_block_skip_ctrls.max_band_cnt;
                uint64_t band_idx =  ctx->md_local_blk_unit[parent_depth_idx_mds].default_cost / band_size;

                in_depth_block_skip_weight = (in_depth_block_skip_weight * ctx->in_depth_block_skip_ctrls.cnt_based_weight[band_idx]) / 100;
            }
            else {
                in_depth_block_skip_weight = 0;
            }
        }

        if (parent_depth_cost != MAX_MODE_COST && parent_depth_cost <=
            current_depth_cost +
            (current_depth_cost * (4 - ctx->blk_geom->quadi) *
                in_depth_block_skip_weight / ctx->blk_geom->quadi / 100)) {
#else
        if (parent_depth_cost != MAX_MODE_COST &&
            parent_depth_cost <= current_depth_cost +
                    (current_depth_cost * (4 - ctx->blk_geom->quadi) * ctx->md_exit_th /
                     ctx->blk_geom->quadi / 100)) {
#endif
#else
        if (parent_depth_cost != MAX_MODE_COST && parent_depth_cost <= current_depth_cost) {
#endif
            *md_early_exit_sq          = 1;
            *next_non_skip_blk_idx_mds = parent_depth_idx_mds +
                ns_depth_offset[scs->seq_header.sb_size == BLOCK_128X128][blk_geom->depth - 1];
        } else {
            *md_early_exit_sq = 0;
        }
    }
    // skip until we reach the next block @ the parent block depth
    if (blk_ptr->mds_idx >= *next_non_skip_blk_idx_mds && *md_early_exit_sq == 1)
        *md_early_exit_sq = 0;
}

/*
 * Check if a block is redundant, and if so, copy the data from the original block
 * return 1 if block is redundant and updated, 0 otherwise
 */
EbBool update_redundant(PictureControlSet *pcs, ModeDecisionContext *ctx) {
    uint8_t          redundant_blk_avail = 0;
    uint16_t         redundant_blk_mds;
    const BlockGeom *blk_geom = ctx->blk_geom;
    BlkStruct *      blk_ptr  = ctx->blk_ptr;

    if (!ctx->md_disallow_nsq)
        check_redundant_block(blk_geom, ctx, &redundant_blk_avail, &redundant_blk_mds);

    ctx->similar_blk_avail = 0;

    if (!ctx->md_disallow_nsq)
        check_similar_block(blk_geom, ctx, &ctx->similar_blk_avail, &ctx->similar_blk_mds);

    if (redundant_blk_avail && ctx->redundant_blk) {
        // Copy results
        BlkStruct *src_cu = &ctx->md_blk_arr_nsq[redundant_blk_mds];
        BlkStruct *dst_cu = blk_ptr;
        move_blk_data_redund(pcs, ctx, src_cu, dst_cu);
        memcpy(&ctx->md_local_blk_unit[blk_ptr->mds_idx],
               &ctx->md_local_blk_unit[redundant_blk_mds],
               sizeof(MdBlkStruct));
        ctx->avail_blk_flag[dst_cu->mds_idx] = ctx->avail_blk_flag[redundant_blk_mds];

        if (!ctx->hbd_mode_decision) {
            memcpy(&ctx->md_local_blk_unit[blk_geom->blkidx_mds].neigh_left_recon[0],
                   &ctx->md_local_blk_unit[redundant_blk_mds].neigh_left_recon[0],
                   128);
            memcpy(&ctx->md_local_blk_unit[blk_geom->blkidx_mds].neigh_left_recon[1],
                   &ctx->md_local_blk_unit[redundant_blk_mds].neigh_left_recon[1],
                   128);
            memcpy(&ctx->md_local_blk_unit[blk_geom->blkidx_mds].neigh_left_recon[2],
                   &ctx->md_local_blk_unit[redundant_blk_mds].neigh_left_recon[2],
                   128);
            memcpy(&ctx->md_local_blk_unit[blk_geom->blkidx_mds].neigh_top_recon[0],
                   &ctx->md_local_blk_unit[redundant_blk_mds].neigh_top_recon[0],
                   128);
            memcpy(&ctx->md_local_blk_unit[blk_geom->blkidx_mds].neigh_top_recon[1],
                   &ctx->md_local_blk_unit[redundant_blk_mds].neigh_top_recon[1],
                   128);
            memcpy(&ctx->md_local_blk_unit[blk_geom->blkidx_mds].neigh_top_recon[2],
                   &ctx->md_local_blk_unit[redundant_blk_mds].neigh_top_recon[2],
                   128);
        } else {
            uint16_t sz = sizeof(uint16_t);
            memcpy(&ctx->md_local_blk_unit[blk_geom->blkidx_mds].neigh_left_recon_16bit[0],
                   &ctx->md_local_blk_unit[redundant_blk_mds].neigh_left_recon_16bit[0],
                   128 * sz);
            memcpy(&ctx->md_local_blk_unit[blk_geom->blkidx_mds].neigh_left_recon_16bit[1],
                   &ctx->md_local_blk_unit[redundant_blk_mds].neigh_left_recon_16bit[1],
                   128 * sz);
            memcpy(&ctx->md_local_blk_unit[blk_geom->blkidx_mds].neigh_left_recon_16bit[2],
                   &ctx->md_local_blk_unit[redundant_blk_mds].neigh_left_recon_16bit[2],
                   128 * sz);
            memcpy(&ctx->md_local_blk_unit[blk_geom->blkidx_mds].neigh_top_recon_16bit[0],
                   &ctx->md_local_blk_unit[redundant_blk_mds].neigh_top_recon_16bit[0],
                   128 * sz);
            memcpy(&ctx->md_local_blk_unit[blk_geom->blkidx_mds].neigh_top_recon_16bit[1],
                   &ctx->md_local_blk_unit[redundant_blk_mds].neigh_top_recon_16bit[1],
                   128 * sz);
            memcpy(&ctx->md_local_blk_unit[blk_geom->blkidx_mds].neigh_top_recon_16bit[2],
                   &ctx->md_local_blk_unit[redundant_blk_mds].neigh_top_recon_16bit[2],
                   128 * sz);
        }

        memcpy(&ctx->md_ep_pipe_sb[blk_ptr->mds_idx],
               &ctx->md_ep_pipe_sb[redundant_blk_mds],
               sizeof(MdEncPassCuData));

        if (blk_geom->shape == PART_N) {
            uint8_t sq_index              = svt_log2f(blk_geom->sq_size) - 2;
            ctx->parent_sq_type[sq_index] = src_cu->prediction_mode_flag;
#if !FTR_NEW_CYCLES_ALLOC
            ctx->parent_sq_has_coeff[sq_index] = src_cu->block_has_coeff;
#endif
            ctx->parent_sq_pred_mode[sq_index] = src_cu->pred_mode;
        }
        return 1;
    }
    return 0;
}

/*
 * Determined if a block should be processed, and if so, perform the MD pass on the block.
 */
void process_block(SequenceControlSet *scs, PictureControlSet *pcs, ModeDecisionContext *ctx,
                   const EbMdcLeafData *const leaf_data_ptr,
#if OPT6_DEPTH_REFINEMENT
                   const uint8_t blk_split_flag,
#endif
                   EbPictureBufferDesc *in_pic,
#if OPT_INIT
                uint32_t sb_addr,
#else
                SuperBlock *sb_ptr, uint32_t sb_addr,
#endif
                   uint16_t sb_org_x, uint16_t sb_org_y, uint32_t blk_idx_mds,
                   uint32_t *next_non_skip_blk_idx_mds, EbBool *md_early_exit_sq,
                   EbBool *md_early_exit_nsq, uint8_t first_d1_blk, uint8_t d1_blk_count) {

    const BlockGeom *blk_geom = ctx->blk_geom = get_blk_geom_mds(blk_idx_mds);
    BlkStruct *      blk_ptr = ctx->blk_ptr = &ctx->md_blk_arr_nsq[blk_idx_mds];
#if !OPT_INIT
    ctx->sb_ptr = sb_ptr;
#endif
#if !CLN_NSQ_AND_STATS
    EbBool skip_processing_block = ctx->blk_ptr->do_not_process_block || (*md_early_exit_nsq);
#endif
    init_block_data(pcs,
                    ctx,
                    leaf_data_ptr,
#if OPT6_DEPTH_REFINEMENT
                    blk_split_flag,
#endif
#if !OPT_INIT_XD
        sb_ptr,
#endif
                    sb_org_x,
                    sb_org_y,
                    blk_idx_mds,
                    first_d1_blk);

    // Reset settings, in case they were over-written by previous block
#if CLN_NSQ_AND_STATS
    // Only reset settings when features that change settings are used.
    if (!ctx->md_disallow_nsq)
#endif
        signal_derivation_enc_dec_kernel_oq(scs, pcs, ctx);
#if OPT_INIT
    signal_derivation_block(ctx);
#else
    signal_derivation_block(pcs, ctx);
#endif
#if !CLN_NSQ_AND_STATS
    // Use more aggressive (faster, but less accurate) settigns for unlikely paritions (incl. SQ)
    update_md_settings_based_on_stats(ctx,
                                      ctx->md_local_blk_unit[blk_idx_mds].pred_depth_refinement);
#endif
#if !FTR_NEW_CYCLES_ALLOC
    // If SQ block has zero coeffs, use more aggressive settings (or skip) for NSQ blocks
    skip_processing_block |= update_md_settings_based_on_sq_coeff(ctx);
#endif
    // Check current depth cost; if larger than parent, exit early
    check_curr_to_parent_cost(
        scs, pcs, ctx, sb_addr, next_non_skip_blk_idx_mds, md_early_exit_sq, d1_blk_count);

    // encode the current block only if it's not redundant
    if (!update_redundant(pcs, ctx)) {
#if CLN_NSQ_AND_STATS
#if OPT_BUILD_CAND_BLK_2
        EbBool skip_processing_block = ctx->do_not_process_blk[blk_idx_mds] ||
            (*md_early_exit_nsq) || (*md_early_exit_sq);
#else
        EbBool skip_processing_block = blk_ptr->do_not_process_block || (*md_early_exit_nsq) ||
            (*md_early_exit_sq);
#endif

        // call nsq-reduction func if NSQ is on
        if (!ctx->md_disallow_nsq) {
#if FTR_NSQ_RED_USING_RECON
            skip_processing_block |= update_skip_nsq_based_on_sq_recon_dist(ctx);
#endif
            skip_processing_block |= update_skip_nsq_shapes(ctx);
            skip_processing_block |= update_md_settings_based_on_sq_coeff_area(ctx);
        }
#else
        skip_processing_block |= (*md_early_exit_sq);
        skip_processing_block |= update_skip_nsq_shapes(ctx);
#if FTR_NEW_CYCLES_ALLOC
        skip_processing_block |= update_md_settings_based_on_sq_coeff_area(ctx);
#else
        skip_processing_block |= get_allowed_block(ctx);
#endif
#endif
        if (!skip_processing_block &&
            pcs->parent_pcs_ptr->sb_geom[sb_addr].block_is_allowed[blk_ptr->mds_idx]) {
            // Encode the block
#if OPT_SB_CLASS
#if OPT6_DEPTH_REFINEMENT
            md_encode_block(pcs, ctx, blk_split_flag, in_pic);
#else
            md_encode_block(pcs, ctx, leaf_data_ptr, in_pic);
#endif
#else
            md_encode_block(pcs, ctx, in_pic);
#endif
        } else if (!pcs->parent_pcs_ptr->sb_geom[sb_addr].block_is_allowed[blk_ptr->mds_idx]) {
            // If the block is out of the boundaries, MD is not performed.
            // - For square blocks, since the blocks can be split further, they are considered in d2_inter_depth_block_decision() with cost of zero.
            // - For non-square blocks, since they can not be further split, the cost is set to the MAX value (MAX_MODE_COST) to ensure they are not selected.
            ctx->md_local_blk_unit[blk_ptr->mds_idx].cost         = (blk_geom->shape != PART_N)
                        ? MAX_MODE_COST >> 4
                        : 0;
            ctx->md_local_blk_unit[blk_ptr->mds_idx].default_cost = (blk_geom->shape != PART_N)
                ? MAX_MODE_COST >> 4
                : 0;
        } else {
            ctx->md_local_blk_unit[blk_ptr->mds_idx].cost         = MAX_MODE_COST >> 4;
            ctx->md_local_blk_unit[blk_ptr->mds_idx].default_cost = MAX_MODE_COST >> 4;
        }
    }
}
#if FTR_IMPROVE_DEPTH_REMOVAL
/*
 * Get the number of total block in a branch
 */
uint32_t get_number_of_blocks(uint32_t block_idx) {
    const BlockGeom *blk_geom      = get_blk_geom_mds(block_idx);
    uint32_t         tot_d1_blocks = blk_geom->sq_size == 128 ? 17
                : blk_geom->sq_size > 8                       ? 25
                : blk_geom->sq_size == 8                      ? 5
                                                              : 1;
    return tot_d1_blocks;
}
/*
 * Mark the blocks of the lower depth to be skipped
 */
static void set_child_to_be_skipped(ModeDecisionContext *context_ptr, uint32_t blk_index,
                                    int32_t sb_size, int8_t depth_step) {
    const BlockGeom *const blk_geom = get_blk_geom_mds(blk_index);

    if (context_ptr->md_blk_arr_nsq[blk_index].split_flag && blk_geom->sq_size > 4) {
        //Set first child to be considered
        uint32_t child_block_idx_1 = blk_index +
            d1_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth];
        uint32_t child1_tot_d1_blocks = get_number_of_blocks(child_block_idx_1);
        for (uint32_t block_1d_idx = 0; block_1d_idx < child1_tot_d1_blocks; block_1d_idx++)
#if OPT_BUILD_CAND_BLK_2
            context_ptr->do_not_process_blk[child_block_idx_1 + block_1d_idx] = 1;
#else
            context_ptr->md_blk_arr_nsq[child_block_idx_1 + block_1d_idx].do_not_process_block = 1;
#endif
        if (depth_step > 1)
            set_child_to_be_skipped(context_ptr, child_block_idx_1, sb_size, depth_step - 1);
        //Set second child to be considered
        uint32_t child_block_idx_2 = child_block_idx_1 +
            ns_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth + 1];
        uint32_t child2_tot_d1_blocks = get_number_of_blocks(child_block_idx_2);
        for (uint32_t block_1d_idx = 0; block_1d_idx < child2_tot_d1_blocks; block_1d_idx++)
#if OPT_BUILD_CAND_BLK_2
            context_ptr->do_not_process_blk[child_block_idx_2 + block_1d_idx] = 1;
#else
            context_ptr->md_blk_arr_nsq[child_block_idx_2 + block_1d_idx].do_not_process_block = 1;
#endif
        if (depth_step > 1)
            set_child_to_be_skipped(context_ptr, child_block_idx_2, sb_size, depth_step - 1);
        //Set third child to be considered
        uint32_t child_block_idx_3 = child_block_idx_2 +
            ns_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth + 1];
        uint32_t child3_tot_d1_blocks = get_number_of_blocks(child_block_idx_3);
        for (uint32_t block_1d_idx = 0; block_1d_idx < child3_tot_d1_blocks; block_1d_idx++)
#if OPT_BUILD_CAND_BLK_2
            context_ptr->do_not_process_blk[child_block_idx_3 + block_1d_idx] = 1;
#else
            context_ptr->md_blk_arr_nsq[child_block_idx_3 + block_1d_idx].do_not_process_block = 1;
#endif
        if (depth_step > 1)
            set_child_to_be_skipped(context_ptr, child_block_idx_3, sb_size, depth_step - 1);
        //Set forth child to be considered
        uint32_t child_block_idx_4 = child_block_idx_3 +
            ns_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth + 1];
        uint32_t child4_tot_d1_blocks = get_number_of_blocks(child_block_idx_4);
        for (uint32_t block_1d_idx = 0; block_1d_idx < child4_tot_d1_blocks; block_1d_idx++)
#if OPT_BUILD_CAND_BLK_2
            context_ptr->do_not_process_blk[child_block_idx_4 + block_1d_idx] = 1;
#else
            context_ptr->md_blk_arr_nsq[child_block_idx_4 + block_1d_idx].do_not_process_block = 1;
#endif
        if (depth_step > 1)
            set_child_to_be_skipped(context_ptr, child_block_idx_4, sb_size, depth_step - 1);
    }
}

void block_based_depth_reduction(SequenceControlSet *scs_ptr, ModeDecisionContext *context_ptr) {
    uint8_t n = 4;
    float   average, variance, std_deviation, sum = 0, sum1 = 0;

    // Compute the sum of all dist
    for (uint8_t q_idx = 0; q_idx < n; q_idx++) {
        sum = sum +
            context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                .rec_dist_per_quadrant[q_idx];
    }
    average = sum / (float)n;

    // Compute variance and standard deviation
    for (uint8_t q_idx = 0; q_idx < n; q_idx++) {
        sum1 = sum1 +
            (float)pow((context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds]
                            .rec_dist_per_quadrant[q_idx] -
                        average),
                       2);
    }
    variance      = sum1 / (float)n;
    std_deviation = (float)sqrt(variance);

#if LOWER_DEPTH_EXIT_CTRL //--
    // Derive the distortion/cost ratio
    uint32_t full_lambda = context_ptr->hbd_mode_decision ?
        context_ptr->full_lambda_md[EB_10_BIT_MD] :
        context_ptr->full_lambda_md[EB_8_BIT_MD];
    uint64_t dist = RDCOST(full_lambda, 0, context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds].full_distortion);
    uint64_t dist_cost_ratio = (dist * 100) / context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds].cost;
    float min_ratio = context_ptr->lower_depth_block_skip_ctrls.min_distortion_cost_ratio;
    float max_ratio = 100;
    float modulated_th = (100 * (dist_cost_ratio - min_ratio)) / (max_ratio - min_ratio);
    float quand_deviation_th =
        (dist_cost_ratio <= min_ratio) ?
        0 :
        (dist_cost_ratio <= max_ratio) ?
#if LOWER_DEPTH_EXIT_CTRL
        (context_ptr->lower_depth_block_skip_ctrls.quad_deviation_th * modulated_th) / 100 :
#else
        (context_ptr->depth_skip_ctrls.quand_deviation_th * modulated_th) / 100 :
#endif
        dist_cost_ratio;
#else
    // Derive the distortion/cost ratio
    uint32_t full_lambda = context_ptr->hbd_mode_decision
        ? context_ptr->full_lambda_md[EB_10_BIT_MD]
        : context_ptr->full_lambda_md[EB_8_BIT_MD];
    uint64_t dist        = RDCOST(
        full_lambda,
        0,
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds].full_distortion);
    uint64_t dist_cost_ratio = (dist * 100) /
        context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds].cost;

    float min_ratio    = 50;
    float max_ratio    = 100;
    float modulated_th = (100 * (dist_cost_ratio - min_ratio)) / (max_ratio - min_ratio);

    float quand_deviation_th = (dist_cost_ratio <= min_ratio) ? 0
        : (dist_cost_ratio <= max_ratio)
        ? (context_ptr->depth_skip_ctrls.quand_deviation_th * modulated_th) / 100
        : dist_cost_ratio;

#endif
    if (std_deviation < quand_deviation_th)

    {
        set_child_to_be_skipped(
            context_ptr,
            context_ptr->blk_geom->sqi_mds,
            scs_ptr->seq_header.sb_size,
#if LOWER_DEPTH_EXIT_CTRL
            context_ptr->lower_depth_block_skip_ctrls.skip_all ? 6 : 1);
#else
            1);
#endif
    }
}
#endif
/*
 * Update d1 data (including d1 decision) after each processed block, determine if should use early exit.
 */
void update_d1_data(PictureControlSet *pcs, ModeDecisionContext *ctx, uint16_t sb_org_x,
                    uint16_t sb_org_y, uint32_t blk_idx_mds, EbBool *skip_next_nsq,
                    uint8_t *d1_blk_count) {
    const BlockGeom *blk_geom = ctx->blk_geom;
    BlkStruct *      blk_ptr  = ctx->blk_ptr;

    *skip_next_nsq = 0;
    if (blk_geom->nsi + 1 == blk_geom->totns) {
        d1_non_square_block_decision(ctx, *d1_blk_count);
        (*d1_blk_count)++;
    } else if (*d1_blk_count) {
        uint64_t tot_cost      = 0;
        uint32_t first_blk_idx = blk_ptr->mds_idx -
            (blk_geom->nsi); //index of first block in this partition
        for (int blk_it = 0; blk_it < blk_geom->nsi + 1; blk_it++)
            tot_cost += ctx->md_local_blk_unit[first_blk_idx + blk_it].cost;

        if (tot_cost > ctx->md_local_blk_unit[blk_geom->sqi_mds].cost)
            *skip_next_nsq = 1;
    }

    if (blk_geom->shape != PART_N) {
        if (blk_geom->nsi + 1 < blk_geom->totns)
            md_update_all_neighbour_arrays(pcs, ctx, blk_idx_mds, sb_org_x, sb_org_y);
        else
            copy_neighbour_arrays( //restore [1] in [0] after done last ns block
                pcs,
                ctx,
                1,
                0,
                blk_geom->sqi_mds,
                sb_org_x,
                sb_org_y);
    }
}

/*
 * Update d2 data (including d2 decision) after processing the last d1 block of a given square.
 */
void update_d2_decision(SequenceControlSet *scs, PictureControlSet *pcs, ModeDecisionContext *ctx,
                        uint32_t sb_addr, uint16_t sb_org_x, uint16_t sb_org_y) {
    uint32_t last_blk_index_mds = d2_inter_depth_block_decision(
        scs,
        pcs,
        ctx,
        ctx->blk_geom->sqi_mds, //input is parent square
        sb_addr);

    if (ctx->md_blk_arr_nsq[last_blk_index_mds].split_flag == EB_FALSE) {
        md_update_all_neighbour_arrays_multiple(
            pcs,
            ctx,
            ctx->md_local_blk_unit[last_blk_index_mds].best_d1_blk,
            sb_org_x,
            sb_org_y);
    }
#if FTR_IMPROVE_DEPTH_REMOVAL
    // Here d1 is already performed but not d2
#if LOWER_DEPTH_EXIT_CTRL
    if (ctx->lower_depth_block_skip_ctrls.enabled &&
#else
    if (ctx->depth_skip_ctrls.enabled &&
#endif
        ctx->md_blk_arr_nsq[ctx->blk_geom->sqi_mds].split_flag ==
            EB_TRUE && // could be further splitted
        ctx->avail_blk_flag[ctx->blk_geom->sqi_mds]) { // valid block

        block_based_depth_reduction(scs, ctx);
    }
#endif
}

/*
 * Loop over all passed blocks in an SB and perform mode decision for each block,
 * then output the optimal mode distribution/partitioning for the given SB.
 *
 * For each block, selects the best mode through multiple MD stages (accuracy increases
 * while the number of mode candidates decreases as you move from one stage to another).
 * Based on the block costs, selects the best partition for a parent block (if NSQ
 * shapes are present). Finally, performs inter-depth decision towards a final partitiioning.
 */
EB_EXTERN EbErrorType mode_decision_sb(SequenceControlSet *scs, PictureControlSet *pcs,
                                       const MdcSbData *const mdc_sb_data, SuperBlock *sb_ptr,
                                       uint16_t sb_org_x, uint16_t sb_org_y, uint32_t sb_addr,
                                       ModeDecisionContext *ctx) {
    EbErrorType return_error = EB_ErrorNone;
#if OPT_INIT
    ctx->sb_ptr = sb_ptr;
#endif

    // Update neighbour arrays for the SB
    update_neighbour_arrays(pcs, ctx);

    // get the input picture; if high bit-depth, pad the input pic
    EbPictureBufferDesc *input_pic = pcs->parent_pcs_ptr->enhanced_picture_ptr;
    if (ctx->hbd_mode_decision) {
        input_pic = pad_hbd_pictures(scs, pcs, ctx, input_pic, sb_org_x, sb_org_y);
    }

    // Initialize variables used to track blocks
    uint32_t                   leaf_count      = mdc_sb_data->leaf_count;
    const EbMdcLeafData *const leaf_data_array = mdc_sb_data->leaf_data_array;

    EbBool   md_early_exit_sq          = 0;
    EbBool   md_early_exit_nsq         = 0;
    uint32_t next_non_skip_blk_idx_mds = 0;

    uint8_t  first_d1_blk         = 1;
    uint8_t  d1_blk_count         = 0;
    uint32_t d1_blocks_accumlated = 0;

    // Iterate over all blocks which are flagged to be considered
    for (uint32_t blk_idx = 0; blk_idx < leaf_count; blk_idx++) {
        uint32_t                   blk_idx_mds   = leaf_data_array[blk_idx].mds_idx;
        const EbMdcLeafData *const leaf_data_ptr = &leaf_data_array[blk_idx];
#if OPT6_DEPTH_REFINEMENT
        const uint8_t blk_split_flag = mdc_sb_data->split_flag[blk_idx];
#endif
        process_block(scs,
                      pcs,
                      ctx,
                      leaf_data_ptr,
#if OPT6_DEPTH_REFINEMENT
                      blk_split_flag,
#endif
                      input_pic,
#if !OPT_INIT
                      sb_ptr,
#endif
                      sb_addr,
                      sb_org_x,
                      sb_org_y,
                      blk_idx_mds,
                      &next_non_skip_blk_idx_mds,
                      &md_early_exit_sq,
                      &md_early_exit_nsq,
                      first_d1_blk,
                      d1_blk_count);

        update_d1_data(
            pcs, ctx, sb_org_x, sb_org_y, blk_idx_mds, &md_early_exit_nsq, &d1_blk_count);

        // Check if the current block is the last at a given d1 level; if so update d2 info
        d1_blocks_accumlated = (first_d1_blk == 1) ? 1 : d1_blocks_accumlated + 1;
        if (d1_blocks_accumlated == leaf_data_ptr->tot_d1_blocks) {
            // Perform d2 inter-depth decision after final d1 block
            update_d2_decision(scs, pcs, ctx, sb_addr, sb_org_x, sb_org_y);

            first_d1_blk = 1;
            d1_blk_count = 0;
        } else if (first_d1_blk) {
            first_d1_blk = 0;
        }
    }

    return return_error;
}
#else
EB_EXTERN EbErrorType mode_decision_sb(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr,
                                       const MdcSbData *const mdcResultTbPtr, SuperBlock *sb_ptr,
                                       uint16_t sb_origin_x, uint16_t sb_origin_y, uint32_t sb_addr,
                                       ModeDecisionContext *context_ptr) {
    EbErrorType return_error = EB_ErrorNone;
#if RFCTR_MD_BLOCK_LOOP
    update_neighbour_arrays(pcs_ptr, context_ptr);
#else
    //printf("sb_origin_x = %d, sb_origin_y = %d\n", sb_origin_x, sb_origin_y);

    uint32_t blk_index;
    ModeDecisionCandidateBuffer *bestcandidate_buffers[5];
    // Pre Intra Search
    uint32_t leaf_count = mdcResultTbPtr->leaf_count;
    const EbMdcLeafData *const leaf_data_array = mdcResultTbPtr->leaf_data_array;
    const uint16_t tile_idx = context_ptr->tile_index;
    context_ptr->sb_ptr = sb_ptr;
    uint32_t full_lambda = context_ptr->hbd_mode_decision
        ? context_ptr->full_sb_lambda_md[EB_10_BIT_MD]
        : context_ptr->full_sb_lambda_md[EB_8_BIT_MD];
    // Mode Decision Neighbor Arrays
    context_ptr->intra_luma_mode_neighbor_array =
        pcs_ptr->md_intra_luma_mode_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
#if !CLN_MDC_CTX // start
    context_ptr->intra_chroma_mode_neighbor_array =
        pcs_ptr->md_intra_chroma_mode_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    context_ptr->mv_neighbor_array =
        pcs_ptr->md_mv_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
#endif
    context_ptr->skip_flag_neighbor_array =
        pcs_ptr->md_skip_flag_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    context_ptr->mode_type_neighbor_array =
        pcs_ptr->md_mode_type_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    context_ptr->leaf_partition_neighbor_array =
        pcs_ptr->mdleaf_partition_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];

    if (!context_ptr->hbd_mode_decision) {
        context_ptr->luma_recon_neighbor_array =
            pcs_ptr->md_luma_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
        context_ptr->cb_recon_neighbor_array =
            pcs_ptr->md_cb_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
        context_ptr->cr_recon_neighbor_array =
            pcs_ptr->md_cr_recon_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    } else {
        context_ptr->luma_recon_neighbor_array16bit =
            pcs_ptr->md_luma_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
        context_ptr->cb_recon_neighbor_array16bit =
            pcs_ptr->md_cb_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
        context_ptr->cr_recon_neighbor_array16bit =
            pcs_ptr->md_cr_recon_neighbor_array16bit[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    }
    context_ptr->luma_dc_sign_level_coeff_neighbor_array =
        pcs_ptr->md_luma_dc_sign_level_coeff_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    context_ptr->cb_dc_sign_level_coeff_neighbor_array =
        pcs_ptr->md_cb_dc_sign_level_coeff_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    context_ptr->cr_dc_sign_level_coeff_neighbor_array =
        pcs_ptr->md_cr_dc_sign_level_coeff_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    context_ptr->txfm_context_array =
        pcs_ptr->md_txfm_context_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    context_ptr->inter_pred_dir_neighbor_array =
        pcs_ptr->md_inter_pred_dir_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    context_ptr->ref_frame_type_neighbor_array =
        pcs_ptr->md_ref_frame_type_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    context_ptr->interpolation_type_neighbor_array =
        pcs_ptr->md_interpolation_type_neighbor_array[MD_NEIGHBOR_ARRAY_INDEX][tile_idx];
    uint32_t d1_block_itr = 0;
    uint32_t d1_first_block = 1;
#endif
    EbPictureBufferDesc *input_picture_ptr = pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr;
    if (context_ptr->hbd_mode_decision) {
#if RFCTR_MD_BLOCK_LOOP
        input_picture_ptr = pad_hbd_pictures(
            scs_ptr, pcs_ptr, context_ptr, input_picture_ptr, sb_origin_x, sb_origin_y);
#else
        const uint32_t input_luma_offset = ((sb_origin_y + input_picture_ptr->origin_y) *
                                            input_picture_ptr->stride_y) +
            (sb_origin_x + input_picture_ptr->origin_x);
        const uint32_t input_bit_inc_luma_offset = ((sb_origin_y + input_picture_ptr->origin_y) *
                                                    input_picture_ptr->stride_bit_inc_y) +
            (sb_origin_x + input_picture_ptr->origin_x);
        const uint32_t input_cb_offset = (((sb_origin_y + input_picture_ptr->origin_y) >> 1) *
                                          input_picture_ptr->stride_cb) +
            ((sb_origin_x + input_picture_ptr->origin_x) >> 1);
        const uint32_t input_bit_inc_cb_offset = (((sb_origin_y + input_picture_ptr->origin_y) >>
                                                   1) *
                                                  input_picture_ptr->stride_bit_inc_cb) +
            ((sb_origin_x + input_picture_ptr->origin_x) >> 1);
        const uint32_t input_cr_offset = (((sb_origin_y + input_picture_ptr->origin_y) >> 1) *
                                          input_picture_ptr->stride_cr) +
            ((sb_origin_x + input_picture_ptr->origin_x) >> 1);
        const uint32_t input_bit_inc_cr_offset = (((sb_origin_y + input_picture_ptr->origin_y) >>
                                                   1) *
                                                  input_picture_ptr->stride_bit_inc_cr) +
            ((sb_origin_x + input_picture_ptr->origin_x) >> 1);

        uint32_t sb_width = MIN(scs_ptr->sb_size_pix,
                                pcs_ptr->parent_pcs_ptr->aligned_width - sb_origin_x);
        uint32_t sb_height = MIN(scs_ptr->sb_size_pix,
                                 pcs_ptr->parent_pcs_ptr->aligned_height - sb_origin_y);

        pack2d_src(input_picture_ptr->buffer_y + input_luma_offset,
                   input_picture_ptr->stride_y,
                   input_picture_ptr->buffer_bit_inc_y + input_bit_inc_luma_offset,
                   input_picture_ptr->stride_bit_inc_y,
                   (uint16_t *)context_ptr->input_sample16bit_buffer->buffer_y,
                   context_ptr->input_sample16bit_buffer->stride_y,
                   sb_width,
                   sb_height);

        pack2d_src(input_picture_ptr->buffer_cb + input_cb_offset,
                   input_picture_ptr->stride_cb,
                   input_picture_ptr->buffer_bit_inc_cb + input_bit_inc_cb_offset,
                   input_picture_ptr->stride_bit_inc_cb,
                   (uint16_t *)context_ptr->input_sample16bit_buffer->buffer_cb,
                   context_ptr->input_sample16bit_buffer->stride_cb,
                   sb_width >> 1,
                   sb_height >> 1);

        pack2d_src(input_picture_ptr->buffer_cr + input_cr_offset,
                   input_picture_ptr->stride_cr,
                   input_picture_ptr->buffer_bit_inc_cr + input_bit_inc_cr_offset,
                   input_picture_ptr->stride_bit_inc_cr,
                   (uint16_t *)context_ptr->input_sample16bit_buffer->buffer_cr,
                   context_ptr->input_sample16bit_buffer->stride_cr,
                   sb_width >> 1,
                   sb_height >> 1);
        // PAD the packed source in incomplete sb up to max SB size
        pad_input_picture_16bit((uint16_t *)context_ptr->input_sample16bit_buffer->buffer_y,
                                context_ptr->input_sample16bit_buffer->stride_y,
                                sb_width,
                                sb_height,
                                scs_ptr->sb_size_pix - sb_width,
                                scs_ptr->sb_size_pix - sb_height);

        pad_input_picture_16bit((uint16_t *)context_ptr->input_sample16bit_buffer->buffer_cb,
                                context_ptr->input_sample16bit_buffer->stride_cb,
                                sb_width >> 1,
                                sb_height >> 1,
                                (scs_ptr->sb_size_pix - sb_width) >> 1,
                                (scs_ptr->sb_size_pix - sb_height) >> 1);

        pad_input_picture_16bit((uint16_t *)context_ptr->input_sample16bit_buffer->buffer_cr,
                                context_ptr->input_sample16bit_buffer->stride_cr,
                                sb_width >> 1,
                                sb_height >> 1,
                                (scs_ptr->sb_size_pix - sb_width) >> 1,
                                (scs_ptr->sb_size_pix - sb_height) >> 1);
        store16bit_input_src(context_ptr->input_sample16bit_buffer,
                             pcs_ptr,
                             sb_origin_x,
                             sb_origin_y,
                             scs_ptr->sb_size_pix,
                             scs_ptr->sb_size_pix);
        //input_picture_ptr = context_ptr->input_sample16bit_buffer;
        if (!use_output_stat(scs_ptr))
            input_picture_ptr = pcs_ptr->input_frame16bit;
#endif
    }

    //CU Loop
#if RFCTR_MD_BLOCK_LOOP
    uint32_t leaf_count = mdcResultTbPtr->leaf_count;
    const EbMdcLeafData *const leaf_data_array = mdcResultTbPtr->leaf_data_array;

    EbBool md_early_exit_sq = 0;
    EbBool md_early_exit_nsq = 0;
    uint32_t next_non_skip_blk_idx_mds = 0;

    uint8_t first_d1_blk = 1;
    uint8_t d1_blk_count = 0;
    uint32_t d1_blocks_accumlated = 0;

    // Iterate over all blocks which are flagged to be considered
    for (uint32_t blk_idx = 0; blk_idx < leaf_count; blk_idx++) {
        uint32_t blk_idx_mds = leaf_data_array[blk_idx].mds_idx;
        const EbMdcLeafData *const leaf_data_ptr = &mdcResultTbPtr->leaf_data_array[blk_idx];

        process_block(scs_ptr,
                      pcs_ptr,
                      context_ptr,
                      leaf_data_ptr,
                      input_picture_ptr,
                      sb_ptr,
                      sb_addr,
                      sb_origin_x,
                      sb_origin_y,
                      blk_idx_mds,
                      &next_non_skip_blk_idx_mds,
                      &md_early_exit_sq,
                      &md_early_exit_nsq,
                      first_d1_blk,
                      d1_blk_count);

        update_d1_data(pcs_ptr,
                       context_ptr,
                       sb_origin_x,
                       sb_origin_y,
                       blk_idx_mds,
                       &md_early_exit_nsq,
                       &d1_blk_count);

        // Check if the current block is the last at a given d1 level; if so update d2 info
        d1_blocks_accumlated = (first_d1_blk == 1) ? 1 : d1_blocks_accumlated + 1;
        if (d1_blocks_accumlated == leaf_data_ptr->tot_d1_blocks) {
            // Perform d2 inter-depth decision after final d1 block
            update_d2_decision(scs_ptr, pcs_ptr, context_ptr, sb_addr, sb_origin_x, sb_origin_y);

            first_d1_blk = 1;
            d1_blk_count = 0;
        } else if (first_d1_blk) {
            first_d1_blk = 0;
        }
    }
#else
    blk_index = 0; //index over mdc array

    uint32_t d1_blocks_accumlated = 0;
    int skip_next_nsq = 0;
    int skip_next_sq = 0;
    uint32_t next_non_skip_blk_idx_mds = 0;
    int64_t depth_cost[NUMBER_OF_DEPTH] = {-1, -1, -1, -1, -1, -1};
    uint64_t nsq_cost[NUMBER_OF_SHAPES] = {MAX_CU_COST,
                                           MAX_CU_COST,
                                           MAX_CU_COST,
                                           MAX_CU_COST,
                                           MAX_CU_COST,
                                           MAX_CU_COST,
                                           MAX_CU_COST,
                                           MAX_CU_COST,
                                           MAX_CU_COST,
                                           MAX_CU_COST};
    Part nsq_shape_table[NUMBER_OF_SHAPES] = {
        PART_N, PART_H, PART_V, PART_HA, PART_HB, PART_VA, PART_VB, PART_H4, PART_V4, PART_S};
    uint8_t skip_next_depth;
    do {
        uint32_t blk_idx_mds = leaf_data_array[blk_index].mds_idx;

        const BlockGeom *blk_geom = context_ptr->blk_geom = get_blk_geom_mds(blk_idx_mds);
        BlkStruct *blk_ptr = context_ptr->blk_ptr = &context_ptr->md_blk_arr_nsq[blk_idx_mds];

        context_ptr->cu_size_log2 = blk_geom->bwidth_log2;
        context_ptr->blk_origin_x = sb_origin_x + blk_geom->origin_x;
        context_ptr->blk_origin_y = sb_origin_y + blk_geom->origin_y;

        const EbMdcLeafData *const leaf_data_ptr = &mdcResultTbPtr->leaf_data_array[blk_index];
        context_ptr->sb_sz = BLOCK_SIZE_64;
        context_ptr->round_origin_x = ((context_ptr->blk_origin_x >> 3) << 3);
        context_ptr->round_origin_y = ((context_ptr->blk_origin_y >> 3) << 3);
        context_ptr->sb_origin_x = sb_origin_x;
        context_ptr->sb_origin_y = sb_origin_y;
        context_ptr->md_local_blk_unit[blk_idx_mds].tested_blk_flag = EB_TRUE;
        context_ptr->md_ep_pipe_sb[blk_idx_mds].merge_cost = 0;
        context_ptr->md_ep_pipe_sb[blk_idx_mds].skip_cost = 0;
        blk_ptr->av1xd->sb_type = blk_geom->bsize;
        blk_ptr->mds_idx = blk_idx_mds;
        context_ptr->md_blk_arr_nsq[blk_idx_mds].mdc_split_flag = (uint16_t)
                                                                      leaf_data_ptr->split_flag;
        context_ptr->md_local_blk_unit[blk_idx_mds].pred_depth_refinement =
            leaf_data_ptr->final_pred_depth_refinement;
        context_ptr->md_local_blk_unit[blk_idx_mds].pred_depth = leaf_data_ptr->final_pred_depth;
        context_ptr->md_blk_arr_nsq[blk_geom->sqi_mds].split_flag = (uint16_t)
                                                                        leaf_data_ptr->split_flag;
        blk_ptr->split_flag =
            (uint16_t)leaf_data_ptr
                ->split_flag; //mdc indicates smallest or non valid CUs with split flag=
        blk_ptr->qindex = context_ptr->qp_index;
        context_ptr->md_local_blk_unit[blk_idx_mds].best_d1_blk = blk_idx_mds;
        if (leaf_data_ptr->tot_d1_blocks != 1) {
            // We need to get the index of the sq_block for each NSQ branch
            if (d1_first_block) {
                copy_neighbour_arrays( //save a clean neigh in [1], encode uses [0], reload the clean in [0] after done last ns block in a partition
                    pcs_ptr,
                    context_ptr,
                    0,
                    1,
                    blk_geom->sqi_mds,
                    sb_origin_x,
                    sb_origin_y);
            }
        }
        int32_t mi_row = context_ptr->blk_origin_y >> MI_SIZE_LOG2;
        int32_t mi_col = context_ptr->blk_origin_x >> MI_SIZE_LOG2;
        int mi_stride = pcs_ptr->parent_pcs_ptr->av1_cm->mi_stride;
        const int32_t offset = mi_row * mi_stride + mi_col;
        blk_ptr->av1xd->mi = pcs_ptr->mi_grid_base + offset;
        ModeInfo *mi_ptr = *blk_ptr->av1xd->mi;
        blk_ptr->av1xd->up_available = (mi_row > sb_ptr->tile_info.mi_row_start);
        blk_ptr->av1xd->left_available = (mi_col > sb_ptr->tile_info.mi_col_start);
        if (blk_ptr->av1xd->up_available)
            blk_ptr->av1xd->above_mbmi = &mi_ptr[-mi_stride].mbmi;
        else
            blk_ptr->av1xd->above_mbmi = NULL;
        if (blk_ptr->av1xd->left_available)
            blk_ptr->av1xd->left_mbmi = &mi_ptr[-1].mbmi;
        else
            blk_ptr->av1xd->left_mbmi = NULL;

        uint8_t redundant_blk_avail = 0;
        uint16_t redundant_blk_mds;
        {
            // Reset settings, in case they were over-written by previous block
            signal_derivation_enc_dec_kernel_oq(scs_ptr, pcs_ptr, context_ptr);
            signal_derivation_block(pcs_ptr, context_ptr);
        }
        // Use more aggressive (faster, but less accurate) settigns for unlikely paritions (incl. SQ)
        update_md_settings_based_on_stats(
            context_ptr, context_ptr->md_local_blk_unit[blk_idx_mds].pred_depth_refinement);
        // If SQ block has zero coeffs, use more aggressive settings (or skip) for NSQ blocks
        uint8_t zero_sq_coeff_skip_action = update_md_settings_based_on_sq_coeff(context_ptr);
        if (!context_ptr->md_disallow_nsq)
            check_redundant_block(blk_geom, context_ptr, &redundant_blk_avail, &redundant_blk_mds);

        context_ptr->similar_blk_avail = 0;
        if (!context_ptr->md_disallow_nsq)
            check_similar_block(blk_geom,
                                context_ptr,
                                &context_ptr->similar_blk_avail,
                                &context_ptr->similar_blk_mds);
        if (redundant_blk_avail && context_ptr->redundant_blk) {
            // Copy results
            BlkStruct *src_cu = &context_ptr->md_blk_arr_nsq[redundant_blk_mds];
            BlkStruct *dst_cu = blk_ptr;
            move_blk_data_redund(pcs_ptr, context_ptr, src_cu, dst_cu);
            context_ptr->avail_blk_flag[dst_cu->mds_idx] =
                context_ptr->avail_blk_flag[redundant_blk_mds];
            svt_memcpy(&context_ptr->md_local_blk_unit[blk_ptr->mds_idx],
                       &context_ptr->md_local_blk_unit[redundant_blk_mds],
                       sizeof(MdBlkStruct));

            if (!context_ptr->hbd_mode_decision) {
                svt_memcpy(&context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                                .neigh_left_recon[0],
                           &context_ptr->md_local_blk_unit[redundant_blk_mds].neigh_left_recon[0],
                           128);
                svt_memcpy(&context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                                .neigh_left_recon[1],
                           &context_ptr->md_local_blk_unit[redundant_blk_mds].neigh_left_recon[1],
                           128);
                svt_memcpy(&context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                                .neigh_left_recon[2],
                           &context_ptr->md_local_blk_unit[redundant_blk_mds].neigh_left_recon[2],
                           128);
                svt_memcpy(&context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                                .neigh_top_recon[0],
                           &context_ptr->md_local_blk_unit[redundant_blk_mds].neigh_top_recon[0],
                           128);
                svt_memcpy(&context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                                .neigh_top_recon[1],
                           &context_ptr->md_local_blk_unit[redundant_blk_mds].neigh_top_recon[1],
                           128);
                svt_memcpy(&context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                                .neigh_top_recon[2],
                           &context_ptr->md_local_blk_unit[redundant_blk_mds].neigh_top_recon[2],
                           128);
            } else {
                uint16_t sz = sizeof(uint16_t);
                svt_memcpy(
                    &context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                         .neigh_left_recon_16bit[0],
                    &context_ptr->md_local_blk_unit[redundant_blk_mds].neigh_left_recon_16bit[0],
                    128 * sz);
                svt_memcpy(
                    &context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                         .neigh_left_recon_16bit[1],
                    &context_ptr->md_local_blk_unit[redundant_blk_mds].neigh_left_recon_16bit[1],
                    128 * sz);
                svt_memcpy(
                    &context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                         .neigh_left_recon_16bit[2],
                    &context_ptr->md_local_blk_unit[redundant_blk_mds].neigh_left_recon_16bit[2],
                    128 * sz);
                svt_memcpy(
                    &context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                         .neigh_top_recon_16bit[0],
                    &context_ptr->md_local_blk_unit[redundant_blk_mds].neigh_top_recon_16bit[0],
                    128 * sz);
                svt_memcpy(
                    &context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                         .neigh_top_recon_16bit[1],
                    &context_ptr->md_local_blk_unit[redundant_blk_mds].neigh_top_recon_16bit[1],
                    128 * sz);
                svt_memcpy(
                    &context_ptr->md_local_blk_unit[context_ptr->blk_geom->blkidx_mds]
                         .neigh_top_recon_16bit[2],
                    &context_ptr->md_local_blk_unit[redundant_blk_mds].neigh_top_recon_16bit[2],
                    128 * sz);
            }

            svt_memcpy(&context_ptr->md_ep_pipe_sb[blk_ptr->mds_idx],
                       &context_ptr->md_ep_pipe_sb[redundant_blk_mds],
                       sizeof(MdEncPassCuData));

            if (context_ptr->blk_geom->shape == PART_N) {
                uint8_t sq_index = svt_log2f(context_ptr->blk_geom->sq_size) - 2;
                context_ptr->parent_sq_type[sq_index] = src_cu->prediction_mode_flag;
                context_ptr->parent_sq_has_coeff[sq_index] = src_cu->block_has_coeff;
                context_ptr->parent_sq_pred_mode[sq_index] = src_cu->pred_mode;
            }
        } else {
            // Initialize tx_depth
            blk_ptr->tx_depth = 0;
            if (blk_geom->quadi > 0 && d1_block_itr == 0 && !skip_next_sq) {
                uint64_t parent_depth_cost = 0, current_depth_cost = 0;
                SequenceControlSet *sqnc_ptr = (SequenceControlSet *)
                                                   pcs_ptr->scs_wrapper_ptr->object_ptr;

                // from a given child index, derive the index of the parent
                uint32_t parent_depth_idx_mds =
                    (context_ptr->blk_geom->sqi_mds -
                     (context_ptr->blk_geom->quadi - 3) *
                         ns_depth_offset[sqnc_ptr->seq_header.sb_size == BLOCK_128X128]
                                        [context_ptr->blk_geom->depth]) -
                    parent_depth_offset[sqnc_ptr->seq_header.sb_size == BLOCK_128X128]
                                       [blk_geom->depth];

                if (pcs_ptr->slice_type == I_SLICE && parent_depth_idx_mds == 0 &&
                    sqnc_ptr->seq_header.sb_size == BLOCK_128X128)
                    parent_depth_cost = MAX_MODE_COST;
                else
                    compute_depth_costs_md_skip(
                        context_ptr,
                        sqnc_ptr,
                        pcs_ptr->parent_pcs_ptr,
                        parent_depth_idx_mds,
                        ns_depth_offset[sqnc_ptr->seq_header.sb_size == BLOCK_128X128]
                                       [context_ptr->blk_geom->depth],
                        &parent_depth_cost,
                        &current_depth_cost);

                if (!pcs_ptr->parent_pcs_ptr->sb_geom[sb_addr]
                         .block_is_allowed[parent_depth_idx_mds])
                    parent_depth_cost = MAX_MODE_COST;

                // compare the cost of the parent to the cost of the already encoded child + an estimated cost for the remaining child @ the current depth
                // if the total child cost is higher than the parent cost then skip the remaining  child @ the current depth
                // when md_exit_th=0 the estimated cost for the remaining child is not taken into account and the action will be lossless compared to no exit
                // MD_EXIT_THSL could be tuned toward a faster encoder but lossy
                if (parent_depth_cost != MAX_MODE_COST && parent_depth_cost <= current_depth_cost) {
                    skip_next_sq = 1;
                    next_non_skip_blk_idx_mds = parent_depth_idx_mds +
                        ns_depth_offset[sqnc_ptr->seq_header.sb_size == BLOCK_128X128]
                                       [context_ptr->blk_geom->depth - 1];
                } else
                    skip_next_sq = 0;
            }
            // skip until we reach the next block @ the parent block depth
            if (blk_ptr->mds_idx >= next_non_skip_blk_idx_mds && skip_next_sq == 1)
                skip_next_sq = 0;

            uint8_t sq_weight_based_nsq_skip = update_skip_nsq_shapes(context_ptr);
            skip_next_depth = context_ptr->blk_ptr->do_not_process_block;
            uint8_t skip_nsq = get_allowed_block(context_ptr);

            if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_addr].block_is_allowed[blk_ptr->mds_idx] &&
                !skip_next_nsq && !skip_next_sq && !sq_weight_based_nsq_skip &&
                !zero_sq_coeff_skip_action && !skip_next_depth && !skip_nsq) {
                md_encode_block(pcs_ptr, context_ptr, input_picture_ptr, bestcandidate_buffers);
            } else if (sq_weight_based_nsq_skip || skip_next_depth || zero_sq_coeff_skip_action) {
                if (context_ptr->blk_geom->shape != PART_N)
                    context_ptr->md_local_blk_unit[context_ptr->blk_ptr->mds_idx].cost =
                        (MAX_MODE_COST >> 4);
                else
                    context_ptr->md_local_blk_unit[context_ptr->blk_ptr->mds_idx].cost =
                        (MAX_MODE_COST >> 10);
                context_ptr->md_local_blk_unit[context_ptr->blk_ptr->mds_idx].default_cost =
                    MAX_MODE_COST;
            } else if (skip_next_sq) {
                context_ptr->md_local_blk_unit[context_ptr->blk_ptr->mds_idx].cost =
                    (MAX_MODE_COST >> 10);
                context_ptr->md_local_blk_unit[context_ptr->blk_ptr->mds_idx].default_cost =
                    MAX_MODE_COST;
            } else {
                // If the block is out of the boundaries, md is not performed.
                // - For square blocks, since the blocks can be further splitted, they are considered in d2_inter_depth_block_decision with cost of zero.
                // - For non square blocks, since they can not be splitted further the cost is set to a large value (MAX_MODE_COST >> 4) to make sure they are not selected.
                //   The value is set to MAX_MODE_COST >> 4 to make sure there is not overflow when adding costs.
                if (context_ptr->blk_geom->shape != PART_N)
                    context_ptr->md_local_blk_unit[context_ptr->blk_ptr->mds_idx].cost =
                        (MAX_MODE_COST >> 4);
                else
                    context_ptr->md_local_blk_unit[context_ptr->blk_ptr->mds_idx].cost = 0;
                if (context_ptr->blk_geom->shape != PART_N)
                    context_ptr->md_local_blk_unit[context_ptr->blk_ptr->mds_idx].default_cost =
                        MAX_MODE_COST;
                else
                    context_ptr->md_local_blk_unit[context_ptr->blk_ptr->mds_idx].default_cost = 0;
            }
        }
        skip_next_nsq = 0;
        if (blk_geom->nsi + 1 == blk_geom->totns) {
            nsq_cost[context_ptr->blk_geom->shape] = d1_non_square_block_decision(context_ptr,
                                                                                  d1_block_itr);
            d1_block_itr++;
        } else if (d1_block_itr) {
            uint64_t tot_cost = 0;
            uint32_t first_blk_idx = context_ptr->blk_ptr->mds_idx -
                (blk_geom->nsi); //index of first block in this partition
            for (int blk_it = 0; blk_it < blk_geom->nsi + 1; blk_it++)
                tot_cost += context_ptr->md_local_blk_unit[first_blk_idx + blk_it].cost;
            nsq_cost[context_ptr->blk_geom->shape] = tot_cost;
            if (tot_cost > context_ptr->md_local_blk_unit[context_ptr->blk_geom->sqi_mds].cost)
                skip_next_nsq = 1;
        }
        if (blk_geom->shape != PART_N) {
            if (blk_geom->nsi + 1 < blk_geom->totns)
                md_update_all_neighbour_arrays(
                    pcs_ptr, context_ptr, blk_idx_mds, sb_origin_x, sb_origin_y);
            else
                copy_neighbour_arrays( //restore [1] in [0] after done last ns block
                    pcs_ptr,
                    context_ptr,
                    1,
                    0,
                    blk_geom->sqi_mds,
                    sb_origin_x,
                    sb_origin_y);
        }
        d1_blocks_accumlated = d1_first_block == 1 ? 1 : d1_blocks_accumlated + 1;
        if (d1_blocks_accumlated == leaf_data_ptr->tot_d1_blocks) {
            //Sorting
            {
                uint32_t i, j, index;
                for (i = 0; i < NUMBER_OF_SHAPES - 1; ++i) {
                    for (j = i + 1; j < NUMBER_OF_SHAPES; ++j) {
                        if (nsq_cost[nsq_shape_table[j]] < nsq_cost[nsq_shape_table[i]]) {
                            index = nsq_shape_table[i];
                            nsq_shape_table[i] = nsq_shape_table[j];
                            nsq_shape_table[j] = index;
                        }
                    }
                }
                depth_cost[scs_ptr->static_config.super_block_size == 128
                               ? context_ptr->blk_geom->depth
                               : context_ptr->blk_geom->depth + 1] += nsq_cost[nsq_shape_table[0]];
            }

            uint32_t last_blk_index_mds = d2_inter_depth_block_decision(
                context_ptr,
                blk_geom->sqi_mds, //input is parent square
                sb_ptr,
                sb_addr,
                sb_origin_x,
                sb_origin_y,
                full_lambda,
                context_ptr->md_rate_estimation_ptr,
                pcs_ptr);
            d1_block_itr = 0;
            d1_first_block = 1;
            if (context_ptr->md_blk_arr_nsq[last_blk_index_mds].split_flag == EB_FALSE) {
                md_update_all_neighbour_arrays_multiple(
                    pcs_ptr,
                    context_ptr,
                    context_ptr->md_local_blk_unit[last_blk_index_mds].best_d1_blk,
                    sb_origin_x,
                    sb_origin_y);
            }
        } else if (d1_first_block)
            d1_first_block = 0;
        blk_index++;
    } while (blk_index < leaf_count); // End of CU loop
    if (scs_ptr->seq_header.sb_size == BLOCK_64X64)
        depth_cost[0] = MAX_CU_COST;

    for (uint8_t depth_idx = 0; depth_idx < NUMBER_OF_DEPTH; depth_idx++) {
        sb_ptr->depth_cost[depth_idx] = depth_cost[depth_idx] < 0 ? MAX_MODE_COST
                                                                  : depth_cost[depth_idx];
    }
#endif
    return return_error;
}
#endif
#define MAX_SEARCH_POINT_WIDTH 128
#define MAX_SEARCH_POINT_HEIGHT 128

#define MAX_TATAL_SEARCH_AREA_WIDTH (MAX_SB_SIZE + MAX_SEARCH_POINT_WIDTH + ME_FILTER_TAP)
#define MAX_TATAL_SEARCH_AREA_HEIGHT (MAX_SB_SIZE + MAX_SEARCH_POINT_HEIGHT + ME_FILTER_TAP)

#define MAX_SEARCH_AREA_SIZE MAX_TATAL_SEARCH_AREA_WIDTH *MAX_TATAL_SEARCH_AREA_HEIGHT
