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

#include "EbEncHandle.h"
#include "EbUtility.h"
#include "EbPictureControlSet.h"
#include "EbModeDecisionConfigurationProcess.h"
#include "EbRateControlResults.h"
#include "EbEncDecTasks.h"
#include "EbReferenceObject.h"
#include "EbModeDecisionProcess.h"
#include "av1me.h"
#include "EbQMatrices.h"
#include "EbLog.h"
#include "EbCoefficients.h"
#include "EbCommonUtils.h"
#include "EbResize.h"

int32_t get_qzbin_factor(int32_t q, AomBitDepth bit_depth);
void    invert_quant(int16_t *quant, int16_t *shift, int32_t d);
int16_t svt_av1_dc_quant_q3(int32_t qindex, int32_t delta, AomBitDepth bit_depth);
int16_t svt_av1_ac_quant_q3(int32_t qindex, int32_t delta, AomBitDepth bit_depth);
int16_t svt_av1_dc_quant_qtx(int32_t qindex, int32_t delta, AomBitDepth bit_depth);

#define MAX_MESH_SPEED 5 // Max speed setting for mesh motion method
static MeshPattern good_quality_mesh_patterns[MAX_MESH_SPEED + 1][MAX_MESH_STEP] = {
    {{64, 8}, {28, 4}, {15, 1}, {7, 1}},
    {{64, 8}, {28, 4}, {15, 1}, {7, 1}},
    {{64, 8}, {14, 2}, {7, 1}, {7, 1}},
    {{64, 16}, {24, 8}, {12, 4}, {7, 1}},
    {{64, 16}, {24, 8}, {12, 4}, {7, 1}},
    {{64, 16}, {24, 8}, {12, 4}, {7, 1}},
};
static unsigned char good_quality_max_mesh_pct[MAX_MESH_SPEED + 1] = {50, 50, 25, 15, 5, 1};
// TODO: These settings are pretty relaxed, tune them for
// each speed setting
static MeshPattern intrabc_mesh_patterns[MAX_MESH_SPEED + 1][MAX_MESH_STEP] = {
    {{256, 1}, {256, 1}, {0, 0}, {0, 0}},
    {{256, 1}, {256, 1}, {0, 0}, {0, 0}},
    {{64, 1}, {64, 1}, {0, 0}, {0, 0}},
    {{64, 1}, {64, 1}, {0, 0}, {0, 0}},
    {{64, 4}, {16, 1}, {0, 0}, {0, 0}},
    {{64, 4}, {16, 1}, {0, 0}, {0, 0}},
};
static uint8_t intrabc_max_mesh_pct[MAX_MESH_SPEED + 1] = {100, 100, 100, 25, 25, 10};

// Adaptive Depth Partitioning
// Shooting states
#define UNDER_SHOOTING 0
#define OVER_SHOOTING 1
#define TBD_SHOOTING 2

#define SB_PRED_OPEN_LOOP_COST 100 // Let's assume PRED_OPEN_LOOP_COST costs ~100 U
#define U_101 101
#define U_102 102
#define U_103 103
#define U_104 104
#define U_105 105
#define U_107 107
#define SB_FAST_OPEN_LOOP_COST 108
#define U_109 109
#define SB_OPEN_LOOP_COST 110 // F_MDC is ~10% slower than PRED_OPEN_LOOP_COST
#define U_111 111
#define U_112 112
#define U_113 113
#define U_114 114
#define U_115 115
#define U_116 116
#define U_117 117
#define U_118 118
#define U_119 119
#define U_120 120
#define U_121 121
#define U_122 122
#define U_125 125
#define U_127 127
#define U_130 130
#define U_132 132
#define U_133 133
#define U_134 134
#define U_140 140
#define U_145 145
#define U_150 150
#define U_152 152
#define SQ_NON4_BLOCKS_SEARCH_COST 155
#define SQ_BLOCKS_SEARCH_COST 190
#define HIGH_SB_SCORE 60000
#define MEDIUM_SB_SCORE 16000
#define LOW_SB_SCORE 6000
#define MAX_LUMINOSITY_BOOST 10
int32_t budget_per_sb_boost[MAX_SUPPORTED_MODES] = {55, 55, 55, 55, 55, 55, 5, 5, 0, 0, 0, 0, 0};

static INLINE int32_t aom_get_qmlevel(int32_t qindex, int32_t first, int32_t last) {
    return first + (qindex * (last + 1 - first)) / QINDEX_RANGE;
}

void set_global_motion_field(PictureControlSet *pcs_ptr) {
    // Init Global Motion Vector
    uint8_t frame_index;
    for (frame_index = INTRA_FRAME; frame_index <= ALTREF_FRAME; ++frame_index) {
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmtype   = IDENTITY;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].alpha    = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].beta     = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].delta    = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].gamma    = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].invalid  = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[0] = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[1] = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[2] = (1 << WARPEDMODEL_PREC_BITS);
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[3] = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[4] = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[5] = (1 << WARPEDMODEL_PREC_BITS);
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[6] = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[7] = 0;
    }

    //Update MV
    PictureParentControlSet *parent_pcs_ptr = pcs_ptr->parent_pcs_ptr;
    for (frame_index = INTRA_FRAME; frame_index <= ALTREF_FRAME; ++frame_index) {
        if (parent_pcs_ptr
                ->is_global_motion[get_list_idx(frame_index)][get_ref_frame_idx(frame_index)])
            parent_pcs_ptr->global_motion[frame_index] =
                parent_pcs_ptr->global_motion_estimation[get_list_idx(frame_index)]
                                                        [get_ref_frame_idx(frame_index)];

        // Upscale the translation parameters by 2, because the search is done on a down-sampled
        // version of the source picture (with a down-sampling factor of 2 in each dimension).
        if (parent_pcs_ptr->gm_level == GM_DOWN16) {
            parent_pcs_ptr->global_motion[frame_index].wmmat[0] *= 4;
            parent_pcs_ptr->global_motion[frame_index].wmmat[1] *= 4;
            parent_pcs_ptr->global_motion[frame_index].wmmat[0] = (int32_t)clamp(
                parent_pcs_ptr->global_motion[frame_index].wmmat[0],
                GM_TRANS_MIN * GM_TRANS_DECODE_FACTOR,
                GM_TRANS_MAX * GM_TRANS_DECODE_FACTOR);
            parent_pcs_ptr->global_motion[frame_index].wmmat[1] = (int32_t)clamp(
                parent_pcs_ptr->global_motion[frame_index].wmmat[1],
                GM_TRANS_MIN * GM_TRANS_DECODE_FACTOR,
                GM_TRANS_MAX * GM_TRANS_DECODE_FACTOR);
        } else if (parent_pcs_ptr->gm_level == GM_DOWN) {
            parent_pcs_ptr->global_motion[frame_index].wmmat[0] *= 2;
            parent_pcs_ptr->global_motion[frame_index].wmmat[1] *= 2;
            parent_pcs_ptr->global_motion[frame_index].wmmat[0] = (int32_t)clamp(
                parent_pcs_ptr->global_motion[frame_index].wmmat[0],
                GM_TRANS_MIN * GM_TRANS_DECODE_FACTOR,
                GM_TRANS_MAX * GM_TRANS_DECODE_FACTOR);
            parent_pcs_ptr->global_motion[frame_index].wmmat[1] = (int32_t)clamp(
                parent_pcs_ptr->global_motion[frame_index].wmmat[1],
                GM_TRANS_MIN * GM_TRANS_DECODE_FACTOR,
                GM_TRANS_MAX * GM_TRANS_DECODE_FACTOR);
        }
    }
}

void svt_av1_set_quantizer(PictureParentControlSet *pcs_ptr, int32_t q) {
    // quantizer has to be reinitialized with av1_init_quantizer() if any
    // delta_q changes.
    FrameHeader *frm_hdr = &pcs_ptr->frm_hdr;

    frm_hdr->quantization_params.using_qmatrix = 0;
    pcs_ptr->min_qmlevel                       = 5;
    pcs_ptr->max_qmlevel                       = 9;

    frm_hdr->quantization_params.base_q_idx = AOMMAX(frm_hdr->delta_q_params.delta_q_present, q);
#if FTR_ENABLE_FIXED_QINDEX_OFFSETS
    if (!pcs_ptr->scs_ptr->static_config.use_fixed_qindex_offsets)
#endif
    {
        frm_hdr->quantization_params.delta_q_dc[AOM_PLANE_Y] = 0;
        frm_hdr->quantization_params.delta_q_ac[AOM_PLANE_Y] = 0;
        frm_hdr->quantization_params.delta_q_ac[AOM_PLANE_U] = 0;
        frm_hdr->quantization_params.delta_q_dc[AOM_PLANE_U] = 0;
        frm_hdr->quantization_params.delta_q_ac[AOM_PLANE_V] = 0;
        frm_hdr->quantization_params.delta_q_dc[AOM_PLANE_V] = 0;
    }

    frm_hdr->quantization_params.qm[AOM_PLANE_Y]         = aom_get_qmlevel(
        frm_hdr->quantization_params.base_q_idx, pcs_ptr->min_qmlevel, pcs_ptr->max_qmlevel);
    frm_hdr->quantization_params.qm[AOM_PLANE_U] = aom_get_qmlevel(
        frm_hdr->quantization_params.base_q_idx +
            frm_hdr->quantization_params.delta_q_ac[AOM_PLANE_U],
        pcs_ptr->min_qmlevel,
        pcs_ptr->max_qmlevel);

    if (!pcs_ptr->separate_uv_delta_q)
        frm_hdr->quantization_params.qm[AOM_PLANE_V] = frm_hdr->quantization_params.qm[AOM_PLANE_U];
    else
        frm_hdr->quantization_params.qm[AOM_PLANE_V] = aom_get_qmlevel(
            frm_hdr->quantization_params.base_q_idx +
                frm_hdr->quantization_params.delta_q_ac[AOM_PLANE_V],
            pcs_ptr->min_qmlevel,
            pcs_ptr->max_qmlevel);
}

void svt_av1_build_quantizer(AomBitDepth bit_depth, int32_t y_dc_delta_q, int32_t u_dc_delta_q,
                             int32_t u_ac_delta_q, int32_t v_dc_delta_q, int32_t v_ac_delta_q,
                             Quants *const quants, Dequants *const deq) {
    int32_t i, q, quant_q3, quant_qtx;

    for (q = 0; q < QINDEX_RANGE; q++) {
        const int32_t qzbin_factor     = get_qzbin_factor(q, bit_depth);
        const int32_t qrounding_factor = q == 0 ? 64 : 48;

        for (i = 0; i < 2; ++i) {
            int32_t qrounding_factor_fp = 64;
            // y quantizer setup with original coeff shift of Q3
            quant_q3 = i == 0 ? svt_av1_dc_quant_q3(q, y_dc_delta_q, bit_depth)
                              : svt_av1_ac_quant_q3(q, 0, bit_depth);
            // y quantizer with TX scale
            quant_qtx = i == 0 ? svt_av1_dc_quant_qtx(q, y_dc_delta_q, bit_depth)
                               : svt_av1_ac_quant_qtx(q, 0, bit_depth);
            invert_quant(&quants->y_quant[q][i], &quants->y_quant_shift[q][i], quant_qtx);
            quants->y_quant_fp[q][i] = (int16_t)((1 << 16) / quant_qtx);
            quants->y_round_fp[q][i] = (int16_t)((qrounding_factor_fp * quant_qtx) >> 7);
            quants->y_zbin[q][i]     = (int16_t)ROUND_POWER_OF_TWO(qzbin_factor * quant_qtx, 7);
            quants->y_round[q][i]    = (int16_t)((qrounding_factor * quant_qtx) >> 7);
            deq->y_dequant_qtx[q][i] = (int16_t)quant_qtx;
            deq->y_dequant_q3[q][i]  = (int16_t)quant_q3;

            // u quantizer setup with original coeff shift of Q3
            quant_q3 = i == 0 ? svt_av1_dc_quant_q3(q, u_dc_delta_q, bit_depth)
                              : svt_av1_ac_quant_q3(q, u_ac_delta_q, bit_depth);
            // u quantizer with TX scale
            quant_qtx = i == 0 ? svt_av1_dc_quant_qtx(q, u_dc_delta_q, bit_depth)
                               : svt_av1_ac_quant_qtx(q, u_ac_delta_q, bit_depth);
            invert_quant(&quants->u_quant[q][i], &quants->u_quant_shift[q][i], quant_qtx);
            quants->u_quant_fp[q][i] = (int16_t)((1 << 16) / quant_qtx);
            quants->u_round_fp[q][i] = (int16_t)((qrounding_factor_fp * quant_qtx) >> 7);
            quants->u_zbin[q][i]     = (int16_t)ROUND_POWER_OF_TWO(qzbin_factor * quant_qtx, 7);
            quants->u_round[q][i]    = (int16_t)((qrounding_factor * quant_qtx) >> 7);
            deq->u_dequant_qtx[q][i] = (int16_t)quant_qtx;
            deq->u_dequant_q3[q][i]  = (int16_t)quant_q3;

            // v quantizer setup with original coeff shift of Q3
            quant_q3 = i == 0 ? svt_av1_dc_quant_q3(q, v_dc_delta_q, bit_depth)
                              : svt_av1_ac_quant_q3(q, v_ac_delta_q, bit_depth);
            // v quantizer with TX scale
            quant_qtx = i == 0 ? svt_av1_dc_quant_qtx(q, v_dc_delta_q, bit_depth)
                               : svt_av1_ac_quant_qtx(q, v_ac_delta_q, bit_depth);
            invert_quant(&quants->v_quant[q][i], &quants->v_quant_shift[q][i], quant_qtx);
            quants->v_quant_fp[q][i] = (int16_t)((1 << 16) / quant_qtx);
            quants->v_round_fp[q][i] = (int16_t)((qrounding_factor_fp * quant_qtx) >> 7);
            quants->v_zbin[q][i]     = (int16_t)ROUND_POWER_OF_TWO(qzbin_factor * quant_qtx, 7);
            quants->v_round[q][i]    = (int16_t)((qrounding_factor * quant_qtx) >> 7);
            deq->v_dequant_qtx[q][i] = (int16_t)quant_qtx;
            deq->v_dequant_q3[q][i]  = (int16_t)quant_q3;
        }

        for (i = 2; i < 8; i++) { // 8: SIMD width
            quants->y_quant[q][i]       = quants->y_quant[q][1];
            quants->y_quant_fp[q][i]    = quants->y_quant_fp[q][1];
            quants->y_round_fp[q][i]    = quants->y_round_fp[q][1];
            quants->y_quant_shift[q][i] = quants->y_quant_shift[q][1];
            quants->y_zbin[q][i]        = quants->y_zbin[q][1];
            quants->y_round[q][i]       = quants->y_round[q][1];
            deq->y_dequant_qtx[q][i]    = deq->y_dequant_qtx[q][1];
            deq->y_dequant_q3[q][i]     = deq->y_dequant_q3[q][1];

            quants->u_quant[q][i]       = quants->u_quant[q][1];
            quants->u_quant_fp[q][i]    = quants->u_quant_fp[q][1];
            quants->u_round_fp[q][i]    = quants->u_round_fp[q][1];
            quants->u_quant_shift[q][i] = quants->u_quant_shift[q][1];
            quants->u_zbin[q][i]        = quants->u_zbin[q][1];
            quants->u_round[q][i]       = quants->u_round[q][1];
            deq->u_dequant_qtx[q][i]    = deq->u_dequant_qtx[q][1];
            deq->u_dequant_q3[q][i]     = deq->u_dequant_q3[q][1];
            quants->v_quant[q][i]       = quants->u_quant[q][1];
            quants->v_quant_fp[q][i]    = quants->v_quant_fp[q][1];
            quants->v_round_fp[q][i]    = quants->v_round_fp[q][1];
            quants->v_quant_shift[q][i] = quants->v_quant_shift[q][1];
            quants->v_zbin[q][i]        = quants->v_zbin[q][1];
            quants->v_round[q][i]       = quants->v_round[q][1];
            deq->v_dequant_qtx[q][i]    = deq->v_dequant_qtx[q][1];
            deq->v_dequant_q3[q][i]     = deq->v_dequant_q3[q][1];
        }
    }
}

void svt_av1_qm_init(PictureParentControlSet *pcs_ptr) {
    const uint8_t num_planes = 3; // MAX_MB_PLANE;// NM- No monochroma
    uint8_t       q, c, t;
    int32_t       current;
    for (q = 0; q < NUM_QM_LEVELS; ++q) {
        for (c = 0; c < num_planes; ++c) {
            current = 0;
            for (t = 0; t < TX_SIZES_ALL; ++t) {
                const int32_t size       = tx_size_2d[t];
                const TxSize  qm_tx_size = av1_get_adjusted_tx_size(t);
                if (q == NUM_QM_LEVELS - 1) {
                    pcs_ptr->gqmatrix[q][c][t]  = NULL;
                    pcs_ptr->giqmatrix[q][c][t] = NULL;
                } else if (t != qm_tx_size) { // Reuse matrices for 'qm_tx_size'
                    pcs_ptr->gqmatrix[q][c][t]  = pcs_ptr->gqmatrix[q][c][qm_tx_size];
                    pcs_ptr->giqmatrix[q][c][t] = pcs_ptr->giqmatrix[q][c][qm_tx_size];
                } else {
                    assert(current + size <= QM_TOTAL_SIZE);
                    pcs_ptr->gqmatrix[q][c][t]  = &wt_matrix_ref[q][c >= 1][current];
                    pcs_ptr->giqmatrix[q][c][t] = &iwt_matrix_ref[q][c >= 1][current];
                    current += size;
                }
            }
        }
    }
}

/******************************************************
* Set the reference sg ep for a given picture
******************************************************/
void set_reference_sg_ep(PictureControlSet *pcs_ptr) {
    Av1Common *        cm = pcs_ptr->parent_pcs_ptr->av1_cm;
    EbReferenceObject *ref_obj_l0, *ref_obj_l1;
    memset(cm->sg_frame_ep_cnt, 0, SGRPROJ_PARAMS * sizeof(int32_t));
    cm->sg_frame_ep = 0;

    // NADER: set cm->sg_ref_frame_ep[0] = cm->sg_ref_frame_ep[1] = -1 to perform all iterations
    switch (pcs_ptr->slice_type) {
    case I_SLICE:
        cm->sg_ref_frame_ep[0] = -1;
        cm->sg_ref_frame_ep[1] = -1;
        break;
    case B_SLICE:
        ref_obj_l0 = (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
        ref_obj_l1 = (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
        cm->sg_ref_frame_ep[0] = ref_obj_l0->sg_frame_ep;
        cm->sg_ref_frame_ep[1] = ref_obj_l1->sg_frame_ep;
        break;
    case P_SLICE:
        ref_obj_l0 = (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
        cm->sg_ref_frame_ep[0] = ref_obj_l0->sg_frame_ep;
        cm->sg_ref_frame_ep[1] = 0;
        break;
    default: SVT_LOG("SG: Not supported picture type"); break;
    }
}

void mode_decision_configuration_init_qp_update(PictureControlSet *pcs_ptr) {
    FrameHeader *frm_hdr                = &pcs_ptr->parent_pcs_ptr->frm_hdr;
    pcs_ptr->parent_pcs_ptr->average_qp = 0;
#if !TUNE_REMOVE_INTRA_STATS_TRACKING
    pcs_ptr->intra_coded_area           = 0;
#endif
    // Init block selection
#if !CLN_REMOVE_UNUSED_CODE
    memset(pcs_ptr->part_cnt, 0, sizeof(uint32_t) * (NUMBER_OF_SHAPES - 1) * FB_NUM * SSEG_NUM);
#endif
#if !CLN_NSQ_AND_STATS
    // Init pred_depth selection
    memset(
        pcs_ptr->pred_depth_count, 0, sizeof(uint32_t) * DEPTH_DELTA_NUM * (NUMBER_OF_SHAPES - 1));
#endif
#if !TUNE_REMOVE_TXT_STATS
    // Init tx_type selection
    memset(pcs_ptr->txt_cnt, 0, sizeof(uint32_t) * TXT_DEPTH_DELTA_NUM * TX_TYPES);
    // Compute Tc, and Beta offsets for a given picture
#endif
    // Set reference sg ep
    set_reference_sg_ep(pcs_ptr);
    set_global_motion_field(pcs_ptr);

    svt_av1_qm_init(pcs_ptr->parent_pcs_ptr);
    MdRateEstimationContext *md_rate_estimation_array;

    md_rate_estimation_array = pcs_ptr->md_rate_estimation_array;

    if (pcs_ptr->parent_pcs_ptr->frm_hdr.primary_ref_frame != PRIMARY_REF_NONE)
        memcpy(&pcs_ptr->md_frame_context,
               &pcs_ptr->ref_frame_context[pcs_ptr->parent_pcs_ptr->frm_hdr.primary_ref_frame],
               sizeof(FRAME_CONTEXT));
    else {
        svt_av1_default_coef_probs(&pcs_ptr->md_frame_context,
                                   frm_hdr->quantization_params.base_q_idx);
        init_mode_probs(&pcs_ptr->md_frame_context);
    }
    // Initial Rate Estimation of the syntax elements
    av1_estimate_syntax_rate(md_rate_estimation_array,
                             pcs_ptr->slice_type == I_SLICE ? EB_TRUE : EB_FALSE,
                             &pcs_ptr->md_frame_context);
    // Initial Rate Estimation of the Motion vectors
    av1_estimate_mv_rate(pcs_ptr, md_rate_estimation_array, &pcs_ptr->md_frame_context);
    // Initial Rate Estimation of the quantized coefficients
    av1_estimate_coefficients_rate(md_rate_estimation_array, &pcs_ptr->md_frame_context);
}

/******************************************************
* Compute Tc, and Beta offsets for a given picture
******************************************************/

static void mode_decision_configuration_context_dctor(EbPtr p) {
    EbThreadContext *                 thread_context_ptr = (EbThreadContext *)p;
    ModeDecisionConfigurationContext *obj                = (ModeDecisionConfigurationContext *)
                                                thread_context_ptr->priv;

    if (obj->is_md_rate_estimation_ptr_owner)
        EB_FREE_ARRAY(obj->md_rate_estimation_ptr);
#if! CLN_CLEANUP_MDC_CTX
    EB_FREE_ARRAY(obj->sb_score_array);
    EB_FREE_ARRAY(obj->sb_cost_array);
    EB_FREE_ARRAY(obj->mdc_candidate_ptr);
    EB_FREE_ARRAY(obj->mdc_ref_mv_stack);
    EB_FREE_ARRAY(obj->mdc_blk_ptr->av1xd);
    EB_FREE_ARRAY(obj->mdc_blk_ptr);
#endif
    EB_FREE_ARRAY(obj);
}
/******************************************************
 * Mode Decision Configuration Context Constructor
 ******************************************************/
EbErrorType mode_decision_configuration_context_ctor(EbThreadContext *  thread_context_ptr,
                                                     const EbEncHandle *enc_handle_ptr,
                                                     int input_index, int output_index) {
#if! CLN_CLEANUP_MDC_CTX
    const SequenceControlSet *scs_ptr = enc_handle_ptr->scs_instance_array[0]->scs_ptr;
    uint32_t sb_total_count           = ((scs_ptr->max_input_luma_width + BLOCK_SIZE_64 - 1) /
                               BLOCK_SIZE_64) *
        ((scs_ptr->max_input_luma_height + BLOCK_SIZE_64 - 1) / BLOCK_SIZE_64);
#endif
    ModeDecisionConfigurationContext *context_ptr;
    EB_CALLOC_ARRAY(context_ptr, 1);
    thread_context_ptr->priv  = context_ptr;
    thread_context_ptr->dctor = mode_decision_configuration_context_dctor;

    // Input/Output System Resource Manager FIFOs
    context_ptr->rate_control_input_fifo_ptr = svt_system_resource_get_consumer_fifo(
        enc_handle_ptr->rate_control_results_resource_ptr, input_index);
    context_ptr->mode_decision_configuration_output_fifo_ptr =
        svt_system_resource_get_producer_fifo(enc_handle_ptr->enc_dec_tasks_resource_ptr,
                                              output_index);
    // Rate estimation
    EB_MALLOC_ARRAY(context_ptr->md_rate_estimation_ptr, 1);
    context_ptr->is_md_rate_estimation_ptr_owner = EB_TRUE;
#if! CLN_CLEANUP_MDC_CTX
    // Adaptive Depth Partitioning
    EB_MALLOC_ARRAY(context_ptr->sb_score_array, sb_total_count);
    EB_MALLOC_ARRAY(context_ptr->sb_cost_array, sb_total_count);

    // Open Loop Partitioning
    EB_MALLOC_ARRAY(context_ptr->mdc_candidate_ptr, 1);
    EB_MALLOC_ARRAY(context_ptr->mdc_ref_mv_stack, 1);
    EB_MALLOC_ARRAY(context_ptr->mdc_blk_ptr, 1);
    context_ptr->mdc_blk_ptr->av1xd = NULL;
    EB_MALLOC_ARRAY(context_ptr->mdc_blk_ptr->av1xd, 1);
#endif
    return EB_ErrorNone;
}

void set_cdf_controls(PictureControlSet *pcs, uint8_t update_cdf_level) {
    CdfControls *ctrl = &pcs->cdf_ctrl;
    switch (update_cdf_level) {
    case 0:
        ctrl->update_mv   = 0;
        ctrl->update_se   = 0;
        ctrl->update_coef = 0;
        break;
    case 1:
        ctrl->update_mv   = 1;
        ctrl->update_se   = 1;
        ctrl->update_coef = 1;
        break;
    case 2:
        ctrl->update_mv   = 0;
        ctrl->update_se   = 1;
        ctrl->update_coef = 1;
        break;
    case 3:
        ctrl->update_mv   = 0;
        ctrl->update_se   = 1;
        ctrl->update_coef = 0;
        break;
    default: assert(0); break;
    }

    ctrl->update_mv = pcs->slice_type == I_SLICE ? 0 : ctrl->update_mv;
    ctrl->enabled   = ctrl->update_coef | ctrl->update_mv | ctrl->update_se;
}
/******************************************************
* Derive Mode Decision Config Settings for OQ
Input   : encoder mode and tune
Output  : EncDec Kernel signal(s)
******************************************************/
EbErrorType signal_derivation_mode_decision_config_kernel_oq(SequenceControlSet *scs_ptr,
                                                             PictureControlSet * pcs_ptr) {
    UNUSED(scs_ptr);
    EbErrorType return_error = EB_ErrorNone;

    uint8_t update_cdf_level = 0;
#if TUNE_LOWER_PRESETS
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_SHIFT_PRESETS_DOWN
    if (pcs_ptr->enc_mode <= ENC_M2)
#else
    if (pcs_ptr->enc_mode <= ENC_M3)
#endif
#else
    if (pcs_ptr->enc_mode <= ENC_M4)
#endif
#else
    if (pcs_ptr->enc_mode <= ENC_M3)
#endif
        update_cdf_level = 1;
#if TUNE_M4_BASE_NBASE && !TUNE_UPDATE_CDF_LEVEL
    else if (pcs_ptr->enc_mode <= ENC_M4)
        update_cdf_level = (pcs_ptr->temporal_layer_index == 0) ? 1 : 0;
#endif
#if !TUNE_M4_M8
    else if (pcs_ptr->enc_mode <= ENC_M5)
        update_cdf_level = 2;
#endif
#if TUNE_UPDATE_CDF_LEVEL
#if TUNE_SHIFT_PRESETS_DOWN
#if TUNE_M0_M8_MEGA_FEB
    else if (pcs_ptr->enc_mode <= ENC_M4)
#else
    else if (pcs_ptr->enc_mode <= ENC_M5)
#endif
#else
    else if (pcs_ptr->enc_mode <= ENC_M6)
#endif
        update_cdf_level = pcs_ptr->slice_type == I_SLICE
        ? 1
        : (pcs_ptr->temporal_layer_index == 0) ? 1 : 3;
#if !TUNE_FINAL_M4_M8
#if TUNE_SHIFT_PRESETS_DOWN
#if TUNE_M0_M8_MEGA_FEB
    else if (pcs_ptr->enc_mode <= ENC_M5)
#else
    else if (pcs_ptr->enc_mode <= ENC_M6)
#endif
#else
    else if (pcs_ptr->enc_mode <= ENC_M7)
#endif
        update_cdf_level = pcs_ptr->slice_type == I_SLICE
        ? 1
        : (pcs_ptr->temporal_layer_index == 0) ? 2 : 3;
#endif
#if TUNE_SHIFT_PRESETS_DOWN
#if TUNE_FINAL_M4_M8
    else if (pcs_ptr->enc_mode <= ENC_M6)
#else
    else if (pcs_ptr->enc_mode <= ENC_M7)
#endif
#else
    else if (pcs_ptr->enc_mode <= ENC_M8)
#endif
        update_cdf_level = pcs_ptr->slice_type == I_SLICE
        ? 1
        : 3;
#endif
#if FTR_M10
#if TUNE_SHIFT_PRESETS_DOWN
    else if (pcs_ptr->enc_mode <= ENC_M8)
#else
    else if (pcs_ptr->enc_mode <= ENC_M9)
#endif
        update_cdf_level = pcs_ptr->slice_type == I_SLICE ? 1 : 0;
    else
        update_cdf_level = 0;
#else
    else
        update_cdf_level = pcs_ptr->slice_type == I_SLICE ? 1 : 0;
#endif

    //set the conrols uisng the required level
    set_cdf_controls(pcs_ptr, update_cdf_level);
    //Filter Intra Mode : 0: OFF  1: ON
    // pic_filter_intra_level specifies whether filter intra would be active
    // for a given picture.

    // pic_filter_intra_level | Settings
    // 0                      | OFF
    // 1                      | ON
    if (scs_ptr->static_config.filter_intra_level == DEFAULT) {
        if (scs_ptr->seq_header.filter_intra_level) {
#if TUNE_M4_M8
#if TUNE_SHIFT_PRESETS_DOWN
            if (pcs_ptr->enc_mode <= ENC_M5)
#else
            if (pcs_ptr->enc_mode <= ENC_M6)
#endif
#else
            if (pcs_ptr->enc_mode <= ENC_M5)
#endif
                pcs_ptr->pic_filter_intra_level = 1;
            else
                pcs_ptr->pic_filter_intra_level = 0;
        } else
            pcs_ptr->pic_filter_intra_level = 0;
    } else
        pcs_ptr->pic_filter_intra_level = scs_ptr->static_config.filter_intra_level;
    FrameHeader *frm_hdr             = &pcs_ptr->parent_pcs_ptr->frm_hdr;
    frm_hdr->allow_high_precision_mv = frm_hdr->quantization_params.base_q_idx <
                HIGH_PRECISION_MV_QTHRESH &&
            (scs_ptr->input_resolution <= INPUT_SIZE_480p_RANGE)
        ? 1
        : 0;
    EbBool enable_wm;
#if TUNE_LOWER_PRESETS
#if TUNE_M4_M8
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_PRESETS_AND_PRUNING
#if TUNE_SHIFT_PRESETS_DOWN
#if TUNE_FINAL_M4_M8
    if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M3) {
#else
    if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M4) {
#endif
#else
    if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M5) {
#endif
#else
    if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M4) {
#endif
#else
    if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M5) {
#endif
#else
    if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M4) {
#endif
#else
    if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M3) {
#endif
        enable_wm = EB_TRUE;
#if TUNE_M9_IFS_SSE_ADAPT_ME_MV_NEAR_WM_TF && !TUNE_M7_M9
    }
    else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M8) {
#else
#if TUNE_SHIFT_PRESETS_DOWN
    } else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M8) {
#else
    } else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M9) {
#endif
#endif
        enable_wm = (pcs_ptr->parent_pcs_ptr->temporal_layer_index == 0) ? EB_TRUE : EB_FALSE;
    } else {
        enable_wm = EB_FALSE;
    }
    if (pcs_ptr->parent_pcs_ptr->scs_ptr->static_config.enable_warped_motion != DEFAULT)
        enable_wm = (EbBool)pcs_ptr->parent_pcs_ptr->scs_ptr->static_config.enable_warped_motion;

    // Note: local warp should be disabled when super-res is ON
    // according to the AV1 spec 5.11.27
    frm_hdr->allow_warped_motion = enable_wm &&
        !(frm_hdr->frame_type == KEY_FRAME || frm_hdr->frame_type == INTRA_ONLY_FRAME) &&
        !frm_hdr->error_resilient_mode && !pcs_ptr->parent_pcs_ptr->frame_superres_enabled;

    frm_hdr->is_motion_mode_switchable = frm_hdr->allow_warped_motion;

    // pic_obmc_level - pic_obmc_level is used to define md_pic_obmc_level.
    // The latter determines the OBMC settings in the function set_obmc_controls.
    // Please check the definitions of the flags/variables in the function
    // set_obmc_controls corresponding to the pic_obmc_level settings.

    //  pic_obmc_level  |              Default Encoder Settings             |     Command Line Settings
    //         0        | OFF subject to possible constraints               | OFF everywhere in encoder
    //         1        | ON subject to possible constraints                | Fully ON in PD_PASS_2
    //         2        | Faster level subject to possible constraints      | Level 2 everywhere in PD_PASS_2
    //         3        | Even faster level subject to possible constraints | Level 3 everywhere in PD_PASS_3
    if (scs_ptr->static_config.obmc_level == DEFAULT) {
#if TUNE_LOWER_PRESETS
#if TUNE_M3_FEATURES
#if TUNE_M4_FEATURES
#if TUNE_SHIFT_PRESETS_DOWN
        if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M3)
#else
        if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M4)
#endif
#else
        if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M3)
#endif
#else
        if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M2)
#endif
#else
        if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M1)
#endif
            pcs_ptr->parent_pcs_ptr->pic_obmc_level = 1;
#if FTR_NEW_REF_PRUNING_CTRLS
#if TUNE_M6_FEATURES
#if TUNE_M6_M7_FEATURES && !TUNE_M0_M8_MEGA_FEB
#if TUNE_SHIFT_PRESETS_DOWN
#if NEW_PRESETS
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M5)
#else
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M4)
#endif
#else
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M5)
#endif
#else
#if TUNE_FINAL_M4_M8
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M5)
#else
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M6)
#endif
#endif
#else
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M5)
#endif
            pcs_ptr->parent_pcs_ptr->pic_obmc_level = 2;
#else
#if TUNE_LOWER_PRESETS
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M5)
#else
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M4)
#endif
            pcs_ptr->parent_pcs_ptr->pic_obmc_level = 2;
#if !TUNE_LOWER_PRESETS
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M5)
            pcs_ptr->parent_pcs_ptr->pic_obmc_level = 3;
#endif
#endif
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_M8_FEATURES
#if TUNE_SHIFT_PRESETS_DOWN
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M7)
#else
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M8)
#endif
#else
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M7)
#endif
            pcs_ptr->parent_pcs_ptr->pic_obmc_level = pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 2 : 0;
#endif
        else
            pcs_ptr->parent_pcs_ptr->pic_obmc_level = 0;
    } else
        pcs_ptr->parent_pcs_ptr->pic_obmc_level = scs_ptr->static_config.obmc_level;

    // Switchable Motion Mode
    frm_hdr->is_motion_mode_switchable = frm_hdr->is_motion_mode_switchable ||
        pcs_ptr->parent_pcs_ptr->pic_obmc_level;

#if !FIX_R2R_10B_LAMBDA
    if (scs_ptr->static_config.enable_hbd_mode_decision == DEFAULT)
#if TUNE_10BIT_MD_SETTINGS
        if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_MR)
            pcs_ptr->hbd_mode_decision = 1;
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M1)
            pcs_ptr->hbd_mode_decision = pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 1 : 2;
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M4)
            pcs_ptr->hbd_mode_decision = 2;
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M7)
            pcs_ptr->hbd_mode_decision = pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 2 : 0;
        else
            pcs_ptr->hbd_mode_decision = pcs_ptr->parent_pcs_ptr->temporal_layer_index == 0 ? 2 : 0;
#else
#if TUNE_HBD_MODE_DECISION
        if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M1)
#else
        if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M0)
#endif
            pcs_ptr->hbd_mode_decision = 1;
        else
            pcs_ptr->hbd_mode_decision = 2;
#endif
    else
        pcs_ptr->hbd_mode_decision = scs_ptr->static_config.enable_hbd_mode_decision;
#endif
#if FTR_REDUCE_MVEST
    pcs_ptr->parent_pcs_ptr->bypass_cost_table_gen = 0;
    if(scs_ptr->input_resolution <= INPUT_SIZE_480p_RANGE)
         pcs_ptr->parent_pcs_ptr->bypass_cost_table_gen = 0;
#if TUNE_FINAL_M4_M8
    else if(pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M7)
#else
    else if(pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M8)
#endif
        pcs_ptr->parent_pcs_ptr->bypass_cost_table_gen = 0;
    //else if(pcs_ptr->slice_type == I_SLICE)
    else if(pcs_ptr->picture_number == 0)
        pcs_ptr->parent_pcs_ptr->bypass_cost_table_gen = 0;
    else
        pcs_ptr->parent_pcs_ptr->bypass_cost_table_gen = 1;
#endif
    return return_error;
}

/******************************************************
* Derive Mode Decision Config Settings for first pass
Input   : encoder mode and tune
Output  : EncDec Kernel signal(s)
******************************************************/
EbErrorType first_pass_signal_derivation_mode_decision_config_kernel(PictureControlSet *pcs_ptr);
void        av1_set_ref_frame(MvReferenceFrame *rf, int8_t ref_frame_type);

static INLINE int get_relative_dist(const OrderHintInfo *oh, int a, int b) {
    if (!oh->enable_order_hint)
        return 0;

    const int bits = oh->order_hint_bits;

    assert(bits >= 1);
    assert(a >= 0 && a < (1 << bits));
    assert(b >= 0 && b < (1 << bits));

    int       diff = a - b;
    const int m    = 1 << (bits - 1);
    diff           = (diff & (m - 1)) - (diff & m);
    return diff;
}

static int get_block_position(Av1Common *cm, int *mi_r, int *mi_c, int blk_row, int blk_col, MV mv,
                              int sign_bias) {
    const int base_blk_row = (blk_row >> 3) << 3;
    const int base_blk_col = (blk_col >> 3) << 3;

    const int row_offset = (mv.row >= 0) ? (mv.row >> (4 + MI_SIZE_LOG2))
                                         : -((-mv.row) >> (4 + MI_SIZE_LOG2));

    const int col_offset = (mv.col >= 0) ? (mv.col >> (4 + MI_SIZE_LOG2))
                                         : -((-mv.col) >> (4 + MI_SIZE_LOG2));

    const int row = (sign_bias == 1) ? blk_row - row_offset : blk_row + row_offset;
    const int col = (sign_bias == 1) ? blk_col - col_offset : blk_col + col_offset;

    if (row < 0 || row >= (cm->mi_rows >> 1) || col < 0 || col >= (cm->mi_cols >> 1))
        return 0;

    if (row < base_blk_row - (MAX_OFFSET_HEIGHT >> 3) ||
        row >= base_blk_row + 8 + (MAX_OFFSET_HEIGHT >> 3) ||
        col < base_blk_col - (MAX_OFFSET_WIDTH >> 3) ||
        col >= base_blk_col + 8 + (MAX_OFFSET_WIDTH >> 3))
        return 0;

    *mi_r = row;
    *mi_c = col;

    return 1;
}

#define MFMV_STACK_SIZE 3

// Note: motion_filed_projection finds motion vectors of current frame's
// reference frame, and projects them to current frame. To make it clear,
// let's call current frame's reference frame as start frame.
// Call Start frame's reference frames as reference frames.
// Call ref_offset as frame distances between start frame and its reference
// frames.
static int motion_field_projection(Av1Common *cm, PictureControlSet *pcs_ptr,
                                   MvReferenceFrame start_frame, int dir) {
    TPL_MV_REF *tpl_mvs_base           = pcs_ptr->tpl_mvs;
    int         ref_offset[REF_FRAMES] = {0};

    uint8_t list_idx0, ref_idx_l0;
    list_idx0  = get_list_idx(start_frame);
    ref_idx_l0 = get_ref_frame_idx(start_frame);
    EbReferenceObject *start_frame_buf =
        (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[list_idx0][ref_idx_l0]->object_ptr;

    if (start_frame_buf == NULL)
        return 0;

    if (start_frame_buf->frame_type == KEY_FRAME || start_frame_buf->frame_type == INTRA_ONLY_FRAME)
        return 0;

    // MFMV is not applied when the reference picture is of a different spatial resolution
    // (described in the AV1 spec section 7.9.2.)
    if (start_frame_buf->mi_rows != cm->mi_rows || start_frame_buf->mi_cols != cm->mi_cols) {
        return 0;
    }

    const int                 start_frame_order_hint        = start_frame_buf->order_hint;
    const unsigned int *const ref_order_hints               = &start_frame_buf->ref_order_hint[0];
    int                       start_to_current_frame_offset = get_relative_dist(
        &pcs_ptr->parent_pcs_ptr->scs_ptr->seq_header.order_hint_info,
        start_frame_order_hint,
        pcs_ptr->parent_pcs_ptr->cur_order_hint);

    for (int i = LAST_FRAME; i <= INTER_REFS_PER_FRAME; ++i)
        ref_offset[i] = get_relative_dist(
            &pcs_ptr->parent_pcs_ptr->scs_ptr->seq_header.order_hint_info,
            start_frame_order_hint,
            ref_order_hints[i - LAST_FRAME]);

    if (dir == 2)
        start_to_current_frame_offset = -start_to_current_frame_offset;

    const MV_REF *const mv_ref_base = start_frame_buf->mvs;
    const int           mvs_rows    = (cm->mi_rows + 1) >> 1;
    const int           mvs_cols    = (cm->mi_cols + 1) >> 1;

    for (int blk_row = 0; blk_row < mvs_rows; ++blk_row) {
        for (int blk_col = 0; blk_col < mvs_cols; ++blk_col) {
            const MV_REF *const mv_ref = &mv_ref_base[blk_row * mvs_cols + blk_col];
            MV                  fwd_mv = mv_ref->mv.as_mv;

            if (mv_ref->ref_frame > INTRA_FRAME) {
                MV        this_mv;
                int       mi_r, mi_c;
                const int ref_frame_offset = ref_offset[mv_ref->ref_frame];

                int pos_valid = abs(ref_frame_offset) <= MAX_FRAME_DISTANCE &&
                    ref_frame_offset > 0 &&
                    abs(start_to_current_frame_offset) <= MAX_FRAME_DISTANCE;

                if (pos_valid) {
                    get_mv_projection(
                        &this_mv, fwd_mv, start_to_current_frame_offset, ref_frame_offset);
                    pos_valid = get_block_position(
                        cm, &mi_r, &mi_c, blk_row, blk_col, this_mv, dir >> 1);
                }

                if (pos_valid) {
                    const int mi_offset = mi_r * (cm->mi_stride >> 1) + mi_c;

                    tpl_mvs_base[mi_offset].mfmv0.as_mv.row  = fwd_mv.row;
                    tpl_mvs_base[mi_offset].mfmv0.as_mv.col  = fwd_mv.col;
                    tpl_mvs_base[mi_offset].ref_frame_offset = ref_frame_offset;
                }
            }
        }
    }

    return 1;
}
static void av1_setup_motion_field(Av1Common *cm, PictureControlSet *pcs_ptr) {
    const OrderHintInfo *const order_hint_info =
        &pcs_ptr->parent_pcs_ptr->scs_ptr->seq_header.order_hint_info;
    memset(pcs_ptr->ref_frame_side, 0, sizeof(pcs_ptr->ref_frame_side));
    if (!order_hint_info->enable_order_hint)
        return;

    TPL_MV_REF *tpl_mvs_base = pcs_ptr->tpl_mvs;
    int         size         = ((cm->mi_rows + MAX_MIB_SIZE) >> 1) * (cm->mi_stride >> 1);

#if ! PIC_BASED_MFMV
    for (int idx = 0; idx < size; ++idx) {
        tpl_mvs_base[idx].mfmv0.as_int     = INVALID_MV;
        tpl_mvs_base[idx].ref_frame_offset = 0;
    }
#endif

    const int                cur_order_hint = pcs_ptr->parent_pcs_ptr->cur_order_hint;
    const EbReferenceObject *ref_buf[INTER_REFS_PER_FRAME];
    int                      ref_order_hint[INTER_REFS_PER_FRAME];

    for (int ref_frame = LAST_FRAME; ref_frame <= ALTREF_FRAME; ref_frame++) {
        const int ref_idx    = ref_frame - LAST_FRAME;
        int       order_hint = 0;
        uint8_t   list_idx0, ref_idx_l0;
        list_idx0  = get_list_idx(ref_frame);
        ref_idx_l0 = get_ref_frame_idx(ref_frame);
        EbReferenceObject *buf =
            (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[list_idx0][ref_idx_l0]->object_ptr;

        if (buf != NULL)
            order_hint = buf->order_hint;

        ref_buf[ref_idx]        = buf;
        ref_order_hint[ref_idx] = order_hint;

        if (get_relative_dist(order_hint_info, order_hint, cur_order_hint) > 0)
            pcs_ptr->ref_frame_side[ref_frame] = 1;
        else if (order_hint == cur_order_hint)
            pcs_ptr->ref_frame_side[ref_frame] = -1;
    }

#if PIC_BASED_MFMV
    //for a frame based mfmv, we need to keep computing the ref_frame_side regardless mfmv is used or no
    if (!pcs_ptr->parent_pcs_ptr->frm_hdr.use_ref_frame_mvs)
        return;


    for (int idx = 0; idx < size; ++idx) {
        tpl_mvs_base[idx].mfmv0.as_int = INVALID_MV;
        tpl_mvs_base[idx].ref_frame_offset = 0;

    }
#endif



    int ref_stamp = MFMV_STACK_SIZE - 1;

    if (ref_buf[LAST_FRAME - LAST_FRAME] != NULL) {
        const int alt_of_lst_order_hint =
            ref_buf[LAST_FRAME - LAST_FRAME]->ref_order_hint[ALTREF_FRAME - LAST_FRAME];
        const int is_lst_overlay = (alt_of_lst_order_hint ==
                                    ref_order_hint[GOLDEN_FRAME - LAST_FRAME]);
        if (!is_lst_overlay)
            motion_field_projection(cm, pcs_ptr, LAST_FRAME, 2);

        --ref_stamp;
    }

    if (get_relative_dist(
            order_hint_info, ref_order_hint[BWDREF_FRAME - LAST_FRAME], cur_order_hint) > 0) {
        if (motion_field_projection(cm, pcs_ptr, BWDREF_FRAME, 0))
            --ref_stamp;
    }

        if (get_relative_dist(
                order_hint_info, ref_order_hint[ALTREF2_FRAME - LAST_FRAME], cur_order_hint) > 0) {
            if (motion_field_projection(cm, pcs_ptr, ALTREF2_FRAME, 0))
                --ref_stamp;
        }

    if (get_relative_dist(
            order_hint_info, ref_order_hint[ALTREF_FRAME - LAST_FRAME], cur_order_hint) > 0 &&
        ref_stamp >= 0)
        if (motion_field_projection(cm, pcs_ptr, ALTREF_FRAME, 0))
            --ref_stamp;

    if (ref_stamp >= 0)
        motion_field_projection(cm, pcs_ptr, LAST2_FRAME, 2);
}

/* Mode Decision Configuration Kernel */

/*********************************************************************************
*
* @brief
*  The Mode Decision Configuration Process involves a number of initialization steps,
*  setting flags for a number of features, and determining the blocks to be considered
*  in subsequent MD stages.
*
* @par Description:
*  The Mode Decision Configuration Process involves a number of initialization steps,
*  setting flags for a number of features, and determining the blocks to be considered
*  in subsequent MD stages. Examples of flags that are set are the flags for filter intra,
*  eighth-pel, OBMC and warped motion and flags for updating the cumulative density functions
*  Examples of initializations include initializations for picture chroma QP offsets,
*  CDEF strength, self-guided restoration filter parameters, quantization parameters,
*  lambda arrays, mv and coefficient rate estimation arrays.
*
*  The set of blocks to be processed in subsequent MD stages is decided in this process as a
*  function of the picture depth mode (pic_depth_mode).
*
* @param[in] Configurations
*  Configuration flags that are to be set
*
* @param[out] Initializations
*  Initializations for various flags and variables
*
********************************************************************************/
void *mode_decision_configuration_kernel(void *input_ptr) {
    // Context & SCS & PCS
    EbThreadContext *                 thread_context_ptr = (EbThreadContext *)input_ptr;
    ModeDecisionConfigurationContext *context_ptr        = (ModeDecisionConfigurationContext *)
                                                        thread_context_ptr->priv;
    // Input
    EbObjectWrapper *rate_control_results_wrapper_ptr;

    // Output
    EbObjectWrapper *enc_dec_tasks_wrapper_ptr;

    for (;;) {
        // Get RateControl Results
        EB_GET_FULL_OBJECT(context_ptr->rate_control_input_fifo_ptr,
                           &rate_control_results_wrapper_ptr);

        RateControlResults *rate_control_results_ptr =
            (RateControlResults *)rate_control_results_wrapper_ptr->object_ptr;
        PictureControlSet *pcs_ptr = (PictureControlSet *)
                                         rate_control_results_ptr->pcs_wrapper_ptr->object_ptr;
        SequenceControlSet *scs_ptr = (SequenceControlSet *)pcs_ptr->scs_wrapper_ptr->object_ptr;

        // -------
        // Scale references if resolution of the reference is different than the input
        // -------
        if (pcs_ptr->parent_pcs_ptr->frame_superres_enabled == 1 &&
            pcs_ptr->slice_type != I_SLICE) {
            if (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag == EB_TRUE &&
                pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr != NULL) {
                // update mi_rows and mi_cols for the reference pic wrapper (used in mfmv for other pictures)
                EbReferenceObject *reference_object =
                    pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr;
                reference_object->mi_rows = pcs_ptr->parent_pcs_ptr->aligned_height >> MI_SIZE_LOG2;
                reference_object->mi_cols = pcs_ptr->parent_pcs_ptr->aligned_width >> MI_SIZE_LOG2;
            }

            scale_rec_references(
                pcs_ptr, pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr, pcs_ptr->hbd_mode_decision);
        }



#if PIC_BASED_MFMV
        if (pcs_ptr->slice_type != I_SLICE && scs_ptr->mfmv_enabled)
#else
        if (pcs_ptr->parent_pcs_ptr->frm_hdr.use_ref_frame_mvs)
#endif
            av1_setup_motion_field(pcs_ptr->parent_pcs_ptr->av1_cm, pcs_ptr);

        FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;

        // Mode Decision Configuration Kernel Signal(s) derivation
        if (use_output_stat(scs_ptr))
            first_pass_signal_derivation_mode_decision_config_kernel(pcs_ptr);
        else
            signal_derivation_mode_decision_config_kernel_oq(scs_ptr, pcs_ptr);

        pcs_ptr->parent_pcs_ptr->average_qp = 0;
#if !TUNE_REMOVE_INTRA_STATS_TRACKING
        pcs_ptr->intra_coded_area           = 0;
#endif
        // Init block selection
#if !CLN_REMOVE_UNUSED_CODE
        memset(pcs_ptr->part_cnt, 0, sizeof(uint32_t) * (NUMBER_OF_SHAPES - 1) * FB_NUM * SSEG_NUM);
#endif
#if !CLN_NSQ_AND_STATS
        // Init pred_depth selection
        memset(pcs_ptr->pred_depth_count,
               0,
               sizeof(uint32_t) * DEPTH_DELTA_NUM * (NUMBER_OF_SHAPES - 1));
#endif
#if !TUNE_REMOVE_TXT_STATS
        // Init tx_type selection
        memset(pcs_ptr->txt_cnt, 0, sizeof(uint32_t) * TXT_DEPTH_DELTA_NUM * TX_TYPES);
        // Compute Tc, and Beta offsets for a given picture
#endif
        // Set reference sg ep
        set_reference_sg_ep(pcs_ptr);
        set_global_motion_field(pcs_ptr);

        svt_av1_qm_init(pcs_ptr->parent_pcs_ptr);
        MdRateEstimationContext *md_rate_estimation_array;

        // QP
        context_ptr->qp = pcs_ptr->picture_qp;

        // QP Index
        context_ptr->qp_index = (uint8_t)frm_hdr->quantization_params.base_q_idx;

        md_rate_estimation_array = pcs_ptr->md_rate_estimation_array;
        // Reset MD rate Estimation table to initial values by copying from md_rate_estimation_array
        if (context_ptr->is_md_rate_estimation_ptr_owner) {
            EB_FREE_ARRAY(context_ptr->md_rate_estimation_ptr);
            context_ptr->is_md_rate_estimation_ptr_owner = EB_FALSE;
        }
        context_ptr->md_rate_estimation_ptr = md_rate_estimation_array;
        if (pcs_ptr->parent_pcs_ptr->frm_hdr.primary_ref_frame != PRIMARY_REF_NONE)
            memcpy(&pcs_ptr->md_frame_context,
                   &pcs_ptr->ref_frame_context[pcs_ptr->parent_pcs_ptr->frm_hdr.primary_ref_frame],
                   sizeof(FRAME_CONTEXT));
        else {
            svt_av1_default_coef_probs(&pcs_ptr->md_frame_context,
                                       frm_hdr->quantization_params.base_q_idx);
            init_mode_probs(&pcs_ptr->md_frame_context);
        }
        // Initial Rate Estimation of the syntax elements
        av1_estimate_syntax_rate(md_rate_estimation_array,
                                 pcs_ptr->slice_type == I_SLICE ? EB_TRUE : EB_FALSE,
                                 &pcs_ptr->md_frame_context);
        // Initial Rate Estimation of the Motion vectors
#if TUNE_FIRSTPASS_LOSSLESS
        if (!use_output_stat(scs_ptr)){
#endif
        av1_estimate_mv_rate(pcs_ptr, md_rate_estimation_array, &pcs_ptr->md_frame_context);
        // Initial Rate Estimation of the quantized coefficients
        av1_estimate_coefficients_rate(md_rate_estimation_array, &pcs_ptr->md_frame_context);
#if TUNE_FIRSTPASS_LOSSLESS
        }
#endif
        if (frm_hdr->allow_intrabc) {
            int            i;
            int            speed          = 1;
            SpeedFeatures *sf             = &pcs_ptr->sf;
            sf->allow_exhaustive_searches = 1;

            const int mesh_speed = AOMMIN(speed, MAX_MESH_SPEED);
            //if (cpi->twopass.fr_content_type == FC_GRAPHICS_ANIMATION)
            //    sf->exhaustive_searches_thresh = (1 << 24);
            //else
            sf->exhaustive_searches_thresh = (1 << 25);

            sf->max_exaustive_pct = good_quality_max_mesh_pct[mesh_speed];
            if (mesh_speed > 0)
                sf->exhaustive_searches_thresh = sf->exhaustive_searches_thresh << 1;

            for (i = 0; i < MAX_MESH_STEP; ++i) {
                sf->mesh_patterns[i].range    = good_quality_mesh_patterns[mesh_speed][i].range;
                sf->mesh_patterns[i].interval = good_quality_mesh_patterns[mesh_speed][i].interval;
            }

            if (pcs_ptr->slice_type == I_SLICE) {
                for (i = 0; i < MAX_MESH_STEP; ++i) {
                    sf->mesh_patterns[i].range    = intrabc_mesh_patterns[mesh_speed][i].range;
                    sf->mesh_patterns[i].interval = intrabc_mesh_patterns[mesh_speed][i].interval;
                }
                sf->max_exaustive_pct = intrabc_max_mesh_pct[mesh_speed];
            }

            {
                // add to hash table
                const int pic_width  = pcs_ptr->parent_pcs_ptr->aligned_width;
                const int pic_height = pcs_ptr->parent_pcs_ptr->aligned_height;

                uint32_t *block_hash_values[2][2];
                int8_t *  is_block_same[2][3];
                int       k, j;

                for (k = 0; k < 2; k++) {
                    for (j = 0; j < 2; j++)
                        block_hash_values[k][j] = malloc(sizeof(uint32_t) * pic_width * pic_height);
                    for (j = 0; j < 3; j++)
                        is_block_same[k][j] = malloc(sizeof(int8_t) * pic_width * pic_height);
                }

                //pcs_ptr->hash_table.p_lookup_table = NULL;
                //svt_av1_hash_table_create(&pcs_ptr->hash_table);

                Yv12BufferConfig cpi_source;
                link_eb_to_aom_buffer_desc_8bit(pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr,
                                                &cpi_source);

                svt_av1_crc_calculator_init(&pcs_ptr->crc_calculator1, 24, 0x5D6DCB);
                svt_av1_crc_calculator_init(&pcs_ptr->crc_calculator2, 24, 0x864CFB);

                svt_av1_generate_block_2x2_hash_value(
                    &cpi_source, block_hash_values[0], is_block_same[0], pcs_ptr);
                svt_av1_generate_block_hash_value(&cpi_source,
                                                  4,
                                                  block_hash_values[0],
                                                  block_hash_values[1],
                                                  is_block_same[0],
                                                  is_block_same[1],
                                                  pcs_ptr);
                svt_av1_add_to_hash_map_by_row_with_precal_data(&pcs_ptr->hash_table,
                                                                block_hash_values[1],
                                                                is_block_same[1][2],
                                                                pic_width,
                                                                pic_height,
                                                                4);
                svt_av1_generate_block_hash_value(&cpi_source,
                                                  8,
                                                  block_hash_values[1],
                                                  block_hash_values[0],
                                                  is_block_same[1],
                                                  is_block_same[0],
                                                  pcs_ptr);
                svt_av1_add_to_hash_map_by_row_with_precal_data(&pcs_ptr->hash_table,
                                                                block_hash_values[0],
                                                                is_block_same[0][2],
                                                                pic_width,
                                                                pic_height,
                                                                8);
                svt_av1_generate_block_hash_value(&cpi_source,
                                                  16,
                                                  block_hash_values[0],
                                                  block_hash_values[1],
                                                  is_block_same[0],
                                                  is_block_same[1],
                                                  pcs_ptr);
                svt_av1_add_to_hash_map_by_row_with_precal_data(&pcs_ptr->hash_table,
                                                                block_hash_values[1],
                                                                is_block_same[1][2],
                                                                pic_width,
                                                                pic_height,
                                                                16);
                svt_av1_generate_block_hash_value(&cpi_source,
                                                  32,
                                                  block_hash_values[1],
                                                  block_hash_values[0],
                                                  is_block_same[1],
                                                  is_block_same[0],
                                                  pcs_ptr);
                svt_av1_add_to_hash_map_by_row_with_precal_data(&pcs_ptr->hash_table,
                                                                block_hash_values[0],
                                                                is_block_same[0][2],
                                                                pic_width,
                                                                pic_height,
                                                                32);
                svt_av1_generate_block_hash_value(&cpi_source,
                                                  64,
                                                  block_hash_values[0],
                                                  block_hash_values[1],
                                                  is_block_same[0],
                                                  is_block_same[1],
                                                  pcs_ptr);
                svt_av1_add_to_hash_map_by_row_with_precal_data(&pcs_ptr->hash_table,
                                                                block_hash_values[1],
                                                                is_block_same[1][2],
                                                                pic_width,
                                                                pic_height,
                                                                64);

                svt_av1_generate_block_hash_value(&cpi_source,
                                                  128,
                                                  block_hash_values[1],
                                                  block_hash_values[0],
                                                  is_block_same[1],
                                                  is_block_same[0],
                                                  pcs_ptr);
                svt_av1_add_to_hash_map_by_row_with_precal_data(&pcs_ptr->hash_table,
                                                                block_hash_values[0],
                                                                is_block_same[0][2],
                                                                pic_width,
                                                                pic_height,
                                                                128);

                for (k = 0; k < 2; k++) {
                    for (j = 0; j < 2; j++) free(block_hash_values[k][j]);
                    for (j = 0; j < 3; j++) free(is_block_same[k][j]);
                }
            }

            svt_av1_init3smotion_compensation(
                &pcs_ptr->ss_cfg, pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr->stride_y);
        }

        // Post the results to the MD processes

        uint16_t tg_count = pcs_ptr->parent_pcs_ptr->tile_group_cols *
            pcs_ptr->parent_pcs_ptr->tile_group_rows;
        for (uint16_t tile_group_idx = 0; tile_group_idx < tg_count; tile_group_idx++) {
            svt_get_empty_object(context_ptr->mode_decision_configuration_output_fifo_ptr,
                                 &enc_dec_tasks_wrapper_ptr);

            EncDecTasks *enc_dec_tasks_ptr = (EncDecTasks *)enc_dec_tasks_wrapper_ptr->object_ptr;
            enc_dec_tasks_ptr->pcs_wrapper_ptr  = rate_control_results_ptr->pcs_wrapper_ptr;
            enc_dec_tasks_ptr->input_type       = ENCDEC_TASKS_MDC_INPUT;
            enc_dec_tasks_ptr->tile_group_index = tile_group_idx;

            // Post the Full Results Object
            svt_post_full_object(enc_dec_tasks_wrapper_ptr);
        }

        // Release Rate Control Results
        svt_release_object(rate_control_results_wrapper_ptr);
    }

    return NULL;
}
