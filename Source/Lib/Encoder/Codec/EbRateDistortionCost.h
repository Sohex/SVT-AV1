/*
* Copyright(c) 2019 Intel Corporation
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#ifndef EbRateDistortionCost_h
#define EbRateDistortionCost_h

/***************************************
 * Includes
 ***************************************/
#include "EbModeDecisionProcess.h"
#include "EbEncIntraPrediction.h"
#include "EbEncInterPrediction.h"
#include "EbLambdaRateTables.h"
#include "EbTransforms.h"
#include "EbEncDecProcess.h"
#include "EbEntropyCoding.h"

#ifdef __cplusplus
extern "C" {
#endif
extern uint64_t svt_av1_cost_coeffs_txb(
#if CLN_FAST_COST
                                       struct ModeDecisionContext *ctx,
#endif
                                        uint8_t allow_update_cdf, FRAME_CONTEXT *ec_ctx,
                                        struct ModeDecisionCandidateBuffer *candidate_buffer_ptr,
                                        const TranLow *const qcoeff, uint16_t eob,
                                        PlaneType plane_type, TxSize transform_size,
                                        TxType transform_type, int16_t txb_skip_ctx,
                                        int16_t dc_sign_ctx, EbBool reduced_transform_set_flag);

#if OPT_INIT_XD
extern void coding_loop_context_generation(PictureControlSet *pcs_ptr,
#else
extern void coding_loop_context_generation(
#endif
    ModeDecisionContext *context_ptr, BlkStruct *blk_ptr, uint32_t blk_origin_x,
    uint32_t blk_origin_y,
#if !CLN_MDC_CTX
    uint32_t sb_sz,
#endif
#if !CLN_MDC_CTX
    NeighborArrayUnit *inter_pred_dir_neighbor_array,
    NeighborArrayUnit *ref_frame_type_neighbor_array,
#endif
#if !CLN_MDC_CTX
    NeighborArrayUnit *intra_luma_mode_neighbor_array,
#endif
    NeighborArrayUnit *skip_flag_neighbor_array,
    NeighborArrayUnit *mode_type_neighbor_array, NeighborArrayUnit *leaf_partition_neighbor_array);

extern EbErrorType av1_txb_calc_cost(
    ModeDecisionCandidate *candidate_ptr, // input parameter, prediction result Ptr
    int16_t                txb_skip_ctx,
    uint32_t               txb_index, // input parameter, TU index inside the CU
    uint32_t
        y_count_non_zero_coeffs, // input parameter, number of non zero Y quantized coefficients
    uint32_t
        cb_count_non_zero_coeffs, // input parameter, number of non zero cb quantized coefficients
    uint32_t
        cr_count_non_zero_coeffs, // input parameter, number of non zero cr quantized coefficients
    uint64_t y_txb_distortion
        [DIST_CALC_TOTAL], // input parameter, Y distortion for both Normal and Cbf zero modes
    uint64_t cb_txb_distortion
        [DIST_CALC_TOTAL], // input parameter, Cb distortion for both Normal and Cbf zero modes
    uint64_t cr_txb_distortion
        [DIST_CALC_TOTAL], // input parameter, Cr distortion for both Normal and Cbf zero modes
    COMPONENT_TYPE component_type,
    uint64_t *     y_txb_coeff_bits, // input parameter, Y quantized coefficients rate
    uint64_t *     cb_txb_coeff_bits, // input parameter, Cb quantized coefficients rate
    uint64_t *     cr_txb_coeff_bits, // input parameter, Cr quantized coefficients rate
    TxSize         txsize,
    uint64_t       lambda); // input parameter, lambda for Luma

extern EbErrorType txb_calc_cost(
    uint32_t cu_size, ModeDecisionCandidate *candidate_ptr, uint32_t txb_index,
    uint32_t transform_size, uint32_t transform_chroma_size, uint32_t y_count_non_zero_coeffs,
    uint32_t cb_count_non_zero_coeffs, uint32_t cr_count_non_zero_coeffs,
    uint64_t y_txb_distortion[DIST_CALC_TOTAL], uint64_t cb_txb_distortion[DIST_CALC_TOTAL],
    uint64_t cr_txb_distortion[DIST_CALC_TOTAL], uint32_t component_mask,
    uint64_t *y_txb_coeff_bits, uint64_t *cb_txb_coeff_bits, uint64_t *cr_txb_coeff_bits,
    uint32_t qp, uint64_t lambda, uint64_t lambda_chroma);
#if FIX_Y_COEFF_FLAG_UPDATE
extern EbErrorType av1_txb_calc_cost_luma(
    uint64_t y_txb_distortion[DIST_CALC_TOTAL], // input parameter, Y distortion for both Normal and Cbf zero modes
    uint64_t *y_txb_coeff_bits,                 // input parameter, Y quantized coefficients rate
    uint64_t *y_full_cost,
    uint64_t lambda);                           // input parameter, lambda for Luma
#else
extern EbErrorType av1_txb_calc_cost_luma(
    int16_t                txb_skip_ctx,
    ModeDecisionCandidate *candidate_ptr, // input parameter, prediction result Ptr
    uint32_t               txb_index, // input parameter, TU index inside the CU
    TxSize                 tx_size,
    uint32_t
        y_count_non_zero_coeffs, // input parameter, number of non zero Y quantized coefficients
    uint64_t y_txb_distortion
        [DIST_CALC_TOTAL], // input parameter, Y distortion for both Normal and Cbf zero modes
    uint64_t *y_txb_coeff_bits, // input parameter, Y quantized coefficients rate
    uint64_t *y_full_cost,
    uint64_t  lambda); // input parameter, lambda for Luma
#endif
extern EbErrorType intra_luma_mode_context(BlkStruct *blk_ptr, uint32_t luma_mode,
                                           int32_t *prediction_index);
extern EbErrorType intra2_nx2_n_fast_cost_islice(
    BlkStruct *blk_ptr, struct ModeDecisionCandidateBuffer *candidate_buffer_ptr, uint32_t qp,
    uint64_t luma_distortion, uint64_t chroma_distortion, uint64_t lambda,
    PictureControlSet *pcs_ptr);
extern EbErrorType merge_skip_full_cost(
    SuperBlock *sb_ptr, BlkStruct *blk_ptr, uint32_t cu_size, uint32_t cu_size_log2,
    ModeDecisionCandidateBuffer *candidate_buffer_ptr, uint32_t qp, uint64_t *y_distortion,
    uint64_t *cb_distortion, uint64_t *cr_distortion, uint64_t lambda, uint64_t lambda_chroma,
    uint64_t *y_coeff_bits, uint64_t *cb_coeff_bits, uint64_t *cr_coeff_bits,
    uint32_t transform_size, uint32_t transform_chroma_size, PictureControlSet *pcs_ptr);
extern EbErrorType split_flag_rate(ModeDecisionContext *context_ptr, BlkStruct *blk_ptr,
                                   uint32_t split_flag, uint64_t *split_rate, uint64_t lambda,
                                   MdRateEstimationContext *md_rate_estimation_ptr,
                                   uint32_t                 tb_max_depth);

#define RDDIV_BITS 7

#define RDCOST(RM, R, D) \
    (ROUND_POWER_OF_TWO(((uint64_t)(R)) * (RM), AV1_PROB_COST_SHIFT) + ((D) * (1 << RDDIV_BITS)))

extern EbErrorType av1_split_flag_rate(PictureParentControlSet *pcs_ptr,
                                       ModeDecisionContext *context_ptr, BlkStruct *blk_ptr,
                                       uint32_t leaf_index, PartitionType partitionType,
                                       uint64_t *split_rate, uint64_t lambda,
                                       MdRateEstimationContext *md_rate_estimation_ptr,
                                       uint32_t                 tb_max_depth);
#if !FTR_SHUT_ENCDEC_CBF_ZERO
extern EbErrorType av1_encode_txb_calc_cost(EncDecContext *context_ptr,
                                            uint32_t *     count_non_zero_coeffs,
                                            uint64_t       y_txb_distortion[DIST_CALC_TOTAL],
                                            uint64_t *y_txb_coeff_bits, uint32_t component_mask);
#endif
extern uint64_t av1_intra_fast_cost(
#if CLN_FAST_COST
                                   struct ModeDecisionContext *context_ptr,
#endif
                                    BlkStruct *blk_ptr, ModeDecisionCandidate *candidate_ptr,
                                    uint32_t qp, uint64_t luma_distortion,
                                    uint64_t chroma_distortion, uint64_t lambda,
                                    PictureControlSet *pcs_ptr, CandidateMv *ref_mv_stack,
                                    const BlockGeom *blk_geom, uint32_t miRow, uint32_t miCol,
                                    uint8_t enable_inter_intra,
#if !CLN_FAST_COST
                                    uint8_t md_pass,
#endif
                                    uint32_t left_neighbor_mode, uint32_t top_neighbor_mode);

extern uint64_t av1_inter_fast_cost(
#if CLN_FAST_COST
                                   struct ModeDecisionContext *context_ptr,
#endif
                                    BlkStruct *blk_ptr, ModeDecisionCandidate *candidate_ptr,
                                    uint32_t qp, uint64_t luma_distortion,
                                    uint64_t chroma_distortion, uint64_t lambda,
                                    PictureControlSet *pcs_ptr, CandidateMv *ref_mv_stack,
                                    const BlockGeom *blk_geom, uint32_t miRow, uint32_t miCol,
                                    uint8_t enable_inter_intra,
#if !CLN_FAST_COST
                                    uint8_t md_pass,
#endif
                                    uint32_t left_neighbor_mode, uint32_t top_neighbor_mode);

extern EbErrorType av1_intra_full_cost(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                                       struct ModeDecisionCandidateBuffer *candidate_buffer_ptr,
                                       BlkStruct *blk_ptr, uint64_t *y_distortion,
                                       uint64_t *cb_distortion, uint64_t *cr_distortion,
                                       uint64_t lambda, uint64_t *y_coeff_bits,
                                       uint64_t *cb_coeff_bits, uint64_t *cr_coeff_bits,
                                       BlockSize bsize);

extern EbErrorType av1_inter_full_cost(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                                       struct ModeDecisionCandidateBuffer *candidate_buffer_ptr,
                                       BlkStruct *blk_ptr, uint64_t *y_distortion,
                                       uint64_t *cb_distortion, uint64_t *cr_distortion,
                                       uint64_t lambda, uint64_t *y_coeff_bits,
                                       uint64_t *cb_coeff_bits, uint64_t *cr_coeff_bits,
                                       BlockSize bsize);
extern uint64_t    get_tx_size_bits(ModeDecisionCandidateBuffer *candidateBuffer,
                                    ModeDecisionContext *context_ptr, PictureControlSet *pcs_ptr,
                                    uint8_t tx_depth, EbBool block_has_coeff);

MvJointType svt_av1_get_mv_joint(const MV *mv);

#ifdef __cplusplus
}
#endif
#endif //EbRateDistortionCost_h
