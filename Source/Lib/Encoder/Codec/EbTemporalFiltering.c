/*
 * Copyright(c) 2019 Netflix, Inc.
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
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "EbTemporalFiltering.h"
#include "EbComputeSAD.h"
#include "EbMotionEstimation.h"
#include "EbMotionEstimationProcess.h"
#include "EbMotionEstimationContext.h"
#include "EbLambdaRateTables.h"
#include "EbPictureAnalysisProcess.h"
#include "EbMcp.h"
#include "av1me.h"
#ifdef ARCH_X86_64
#include <xmmintrin.h>
#endif
#include "EbObject.h"
#include "EbEncInterPrediction.h"
#if 0 //!FTR_PRUNED_SUBPEL_TREE // file was removed as part of this macro's changes
#include "EbComputeVariance_C.h"
#endif
#include "EbLog.h"
#include <limits.h>
#undef _MM_HINT_T2
#define _MM_HINT_T2 1

#if FTR_TPL_TR
#include "EbPictureDecisionResults.h"

void fill_me_pcs_wraper(
    PictureParentControlSet *pcs,
    MePcs *me_pcs,
    uint32_t                 trail_path,
    PictureDecisionResults  *in_results);
#endif
static const uint32_t subblock_xy_16x16[N_16X16_BLOCKS][2] = {{0, 0},
                                                              {0, 1},
                                                              {0, 2},
                                                              {0, 3},
                                                              {1, 0},
                                                              {1, 1},
                                                              {1, 2},
                                                              {1, 3},
                                                              {2, 0},
                                                              {2, 1},
                                                              {2, 2},
                                                              {2, 3},
                                                              {3, 0},
                                                              {3, 1},
                                                              {3, 2},
                                                              {3, 3}};
#if TUNE_REDESIGN_TF_CTRLS
static const uint32_t idx_32x32_to_idx_16x16[4][4] = {
    { 0,  1,  4,  5},
    { 2,  3,  6,  7},
    { 8,  9, 12, 13},
    {10, 11, 14, 15}};
#else
static const uint32_t index_16x16_from_subindexes[4][4]    = {
    {0, 1, 4, 5}, {2, 3, 6, 7}, {8, 9, 12, 13}, {10, 11, 14, 15}};
#endif

extern AomVarianceFnPtr mefn_ptr[BlockSizeS_ALL];

// save YUV to file - auxiliary function for debug
void save_YUV_to_file(char *filename, EbByte buffer_y, EbByte buffer_u, EbByte buffer_v,
                      uint16_t width, uint16_t height, uint16_t stride_y, uint16_t stride_u,
                      uint16_t stride_v, uint16_t origin_y, uint16_t origin_x, uint32_t ss_x,
                      uint32_t ss_y) {
    FILE *fid;

    // save current source picture to a YUV file
    FOPEN(fid, filename, "wb");

    if (!fid) {
        SVT_LOG("Unable to open file %s to write.\n", "temp_picture.yuv");
    } else {
        // the source picture saved in the enchanced_picture_ptr contains a border in x and y dimensions
        EbByte pic_point = buffer_y + (origin_y * stride_y) + origin_x;
        for (int h = 0; h < height; h++) {
            fwrite(pic_point, 1, (size_t)width, fid);
            pic_point = pic_point + stride_y;
        }
        pic_point = buffer_u + ((origin_y >> ss_y) * stride_u) + (origin_x >> ss_x);
        for (int h = 0; h < (height >> ss_y); h++) {
            fwrite(pic_point, 1, (size_t)width >> ss_x, fid);
            pic_point = pic_point + stride_u;
        }
        pic_point = buffer_v + ((origin_y >> ss_y) * stride_v) + (origin_x >> ss_x);
        for (int h = 0; h < (height >> ss_y); h++) {
            fwrite(pic_point, 1, (size_t)width >> ss_x, fid);
            pic_point = pic_point + stride_v;
        }
        fclose(fid);
    }
}

// save YUV to file - auxiliary function for debug
void save_YUV_to_file_highbd(char *filename, uint16_t *buffer_y, uint16_t *buffer_u,
                             uint16_t *buffer_v, uint16_t width, uint16_t height, uint16_t stride_y,
                             uint16_t stride_u, uint16_t stride_v, uint16_t origin_y,
                             uint16_t origin_x, uint32_t ss_x, uint32_t ss_y) {
    FILE *fid;

    // save current source picture to a YUV file
    FOPEN(fid, filename, "wb");

    if (!fid) {
        SVT_LOG("Unable to open file %s to write.\n", "temp_picture.yuv");
    } else {
        // the source picture saved in the enchanced_picture_ptr contains a border in x and y dimensions
        uint16_t *pic_point = buffer_y + (origin_y * stride_y) + origin_x;
        for (int h = 0; h < height; h++) {
            fwrite(pic_point, 2, (size_t)width, fid);
            pic_point = pic_point + stride_y;
        }
        pic_point = buffer_u + ((origin_y >> ss_y) * stride_u) + (origin_x >> ss_x);
        for (int h = 0; h < (height >> ss_y); h++) {
            fwrite(pic_point, 2, (size_t)width >> ss_x, fid);

            pic_point = pic_point + stride_u;
        }
        pic_point = buffer_v + ((origin_y >> ss_y) * stride_v) + (origin_x >> ss_x);
        for (int h = 0; h < (height >> ss_y); h++) {
            fwrite(pic_point, 2, (size_t)width >> ss_x, fid);
            pic_point = pic_point + stride_v;
        }
        fclose(fid);
    }
}

void pack_highbd_pic(const EbPictureBufferDesc *pic_ptr, uint16_t *buffer_16bit[3], uint32_t ss_x,
                     uint32_t ss_y, EbBool include_padding) {
    uint32_t input_y_offset          = 0;
    uint32_t input_bit_inc_y_offset  = 0;
    uint32_t input_cb_offset         = 0;
    uint32_t input_bit_inc_cb_offset = 0;
    uint32_t input_cr_offset         = 0;
    uint32_t input_bit_inc_cr_offset = 0;
    uint16_t width                   = pic_ptr->stride_y;
    uint16_t height                  = (uint16_t)(pic_ptr->origin_y * 2 + pic_ptr->height);

    if (!include_padding) {
        input_y_offset         = ((pic_ptr->origin_y) * pic_ptr->stride_y) + (pic_ptr->origin_x);
        input_bit_inc_y_offset = ((pic_ptr->origin_y) * pic_ptr->stride_bit_inc_y) +
            (pic_ptr->origin_x);
        input_cb_offset = (((pic_ptr->origin_y) >> ss_y) * pic_ptr->stride_cb) +
            ((pic_ptr->origin_x) >> ss_x);
        input_bit_inc_cb_offset = (((pic_ptr->origin_y) >> ss_y) * pic_ptr->stride_bit_inc_cb) +
            ((pic_ptr->origin_x) >> ss_x);
        input_cr_offset = (((pic_ptr->origin_y) >> ss_y) * pic_ptr->stride_cr) +
            ((pic_ptr->origin_x) >> ss_x);
        input_bit_inc_cr_offset = (((pic_ptr->origin_y) >> ss_y) * pic_ptr->stride_bit_inc_cr) +
            ((pic_ptr->origin_x) >> ss_x);

        width  = pic_ptr->width;
        height = pic_ptr->height;
    }

    pack2d_src(pic_ptr->buffer_y + input_y_offset,
               pic_ptr->stride_y,
               pic_ptr->buffer_bit_inc_y + input_bit_inc_y_offset,
               pic_ptr->stride_bit_inc_y,
               buffer_16bit[C_Y],
               pic_ptr->stride_y,
               width,
               height);

    pack2d_src(pic_ptr->buffer_cb + input_cb_offset,
               pic_ptr->stride_cb,
               pic_ptr->buffer_bit_inc_cb + input_bit_inc_cb_offset,
               pic_ptr->stride_bit_inc_cb,
               buffer_16bit[C_U],
               pic_ptr->stride_cb,
               width >> ss_x,
               height >> ss_y);

    pack2d_src(pic_ptr->buffer_cr + input_cr_offset,
               pic_ptr->stride_cr,
               pic_ptr->buffer_bit_inc_cr + input_bit_inc_cr_offset,
               pic_ptr->stride_bit_inc_cr,
               buffer_16bit[C_V],
               pic_ptr->stride_cr,
               width >> ss_x,
               height >> ss_y);
}

void unpack_highbd_pic(uint16_t *buffer_highbd[3], EbPictureBufferDesc *pic_ptr, uint32_t ss_x,
                       uint32_t ss_y, EbBool include_padding) {
    uint32_t input_y_offset          = 0;
    uint32_t input_bit_inc_y_offset  = 0;
    uint32_t input_cb_offset         = 0;
    uint32_t input_bit_inc_cb_offset = 0;
    uint32_t input_cr_offset         = 0;
    uint32_t input_bit_inc_cr_offset = 0;
    uint16_t width                   = pic_ptr->stride_y;
    uint16_t height                  = (uint16_t)(pic_ptr->origin_y * 2 + pic_ptr->height);

    if (!include_padding) {
        input_y_offset         = ((pic_ptr->origin_y) * pic_ptr->stride_y) + (pic_ptr->origin_x);
        input_bit_inc_y_offset = ((pic_ptr->origin_y) * pic_ptr->stride_bit_inc_y) +
            (pic_ptr->origin_x);
        input_cb_offset = (((pic_ptr->origin_y) >> ss_y) * pic_ptr->stride_cb) +
            ((pic_ptr->origin_x) >> ss_x);
        input_bit_inc_cb_offset = (((pic_ptr->origin_y) >> ss_y) * pic_ptr->stride_bit_inc_cb) +
            ((pic_ptr->origin_x) >> ss_x);
        input_cr_offset = (((pic_ptr->origin_y) >> ss_y) * pic_ptr->stride_cr) +
            ((pic_ptr->origin_x) >> ss_x);
        input_bit_inc_cr_offset = (((pic_ptr->origin_y) >> ss_y) * pic_ptr->stride_bit_inc_cr) +
            ((pic_ptr->origin_x) >> ss_x);

        width  = pic_ptr->width;
        height = pic_ptr->height;
    }

    un_pack2d(buffer_highbd[C_Y],
              pic_ptr->stride_y,
              pic_ptr->buffer_y + input_y_offset,
              pic_ptr->stride_y,
              pic_ptr->buffer_bit_inc_y + input_bit_inc_y_offset,
              pic_ptr->stride_bit_inc_y,
              width,
              height);

    un_pack2d(buffer_highbd[C_U],
              pic_ptr->stride_cb,
              pic_ptr->buffer_cb + input_cb_offset,
              pic_ptr->stride_cb,
              pic_ptr->buffer_bit_inc_cb + input_bit_inc_cb_offset,
              pic_ptr->stride_bit_inc_cb,
              width >> ss_x,
              height >> ss_y);

    un_pack2d(buffer_highbd[C_V],
              pic_ptr->stride_cr,
              pic_ptr->buffer_cr + input_cr_offset,
              pic_ptr->stride_cr,
              pic_ptr->buffer_bit_inc_cr + input_bit_inc_cr_offset,
              pic_ptr->stride_bit_inc_cr,
              width >> ss_x,
              height >> ss_y);
}

void generate_padding_pic(EbPictureBufferDesc *pic_ptr, uint32_t ss_x, uint32_t ss_y,
                          EbBool is_highbd) {
    if (!is_highbd) {
        generate_padding(pic_ptr->buffer_cb,
                         pic_ptr->stride_cb,
                         pic_ptr->width >> ss_x,
                         pic_ptr->height >> ss_y,
                         pic_ptr->origin_x >> ss_x,
                         pic_ptr->origin_y >> ss_y);

        generate_padding(pic_ptr->buffer_cr,
                         pic_ptr->stride_cr,
                         pic_ptr->width >> ss_x,
                         pic_ptr->height >> ss_y,
                         pic_ptr->origin_x >> ss_x,
                         pic_ptr->origin_y >> ss_y);
    } else {
        generate_padding(pic_ptr->buffer_cb,
                         pic_ptr->stride_cb,
                         pic_ptr->width >> ss_x,
                         pic_ptr->height >> ss_y,
                         pic_ptr->origin_x >> ss_x,
                         pic_ptr->origin_y >> ss_y);

        generate_padding(pic_ptr->buffer_cr,
                         pic_ptr->stride_cr,
                         pic_ptr->width >> ss_x,
                         pic_ptr->height >> ss_y,
                         pic_ptr->origin_x >> ss_x,
                         pic_ptr->origin_y >> ss_y);

        generate_padding(pic_ptr->buffer_bit_inc_cb,
                         pic_ptr->stride_cr,
                         pic_ptr->width >> ss_x,
                         pic_ptr->height >> ss_y,
                         pic_ptr->origin_x >> ss_x,
                         pic_ptr->origin_y >> ss_y);

        generate_padding(pic_ptr->buffer_bit_inc_cr,
                         pic_ptr->stride_cr,
                         pic_ptr->width >> ss_x,
                         pic_ptr->height >> ss_y,
                         pic_ptr->origin_x >> ss_x,
                         pic_ptr->origin_y >> ss_y);
    }
}
static void derive_tf_32x32_block_split_flag(MeContext *context_ptr) {
    int subblock_errors[4];
#if TUNE_REDESIGN_TF_CTRLS
    uint32_t idx_32x32 = context_ptr->idx_32x32;
#else
    for (uint32_t idx_32x32 = 0; idx_32x32 < 4; idx_32x32++) {

        if (!context_ptr->tf_16x16_search_do[idx_32x32]) {
            context_ptr->tf_32x32_block_split_flag[idx_32x32] = 0;
            continue;
        }
#endif
        int block_error = (int)context_ptr->tf_32x32_block_error[idx_32x32];

        // `block_error` is initialized as INT_MAX and will be overwritten after
        // motion search with reference frame, therefore INT_MAX can ONLY be accessed
        // by to-filter frame.
        if (block_error == INT_MAX) {
            context_ptr->tf_32x32_block_split_flag[idx_32x32] = 0;
        }

        int min_subblock_error = INT_MAX;
        int max_subblock_error = INT_MIN;
        int sum_subblock_error = 0;
        for (int i = 0; i < 4; ++i) {
            subblock_errors[i] = (int)context_ptr->tf_16x16_block_error[idx_32x32 * 4 + i];

            sum_subblock_error += subblock_errors[i];
            min_subblock_error = AOMMIN(min_subblock_error, subblock_errors[i]);
            max_subblock_error = AOMMAX(max_subblock_error, subblock_errors[i]);
        }

        if (((block_error * 15 < sum_subblock_error * 16) &&
             max_subblock_error - min_subblock_error < 12000) ||
            ((block_error * 14 < sum_subblock_error * 16) &&
             max_subblock_error - min_subblock_error < 6000)) { // No split.
            context_ptr->tf_32x32_block_split_flag[idx_32x32] = 0;
        } else { // Do split.
            context_ptr->tf_32x32_block_split_flag[idx_32x32] = 1;
        }
#if !TUNE_REDESIGN_TF_CTRLS
    }
#endif
}
// Create and initialize all necessary ME context structures
static void create_me_context_and_picture_control(
    MotionEstimationContext_t *context_ptr, PictureParentControlSet *picture_control_set_ptr_frame,
    PictureParentControlSet *picture_control_set_ptr_central,
    EbPictureBufferDesc *input_picture_ptr_central, int blk_row, int blk_col, uint32_t ss_x,
    uint32_t ss_y) {
#if  !SS_OPT_TF2_ME_COPY
    uint32_t sb_row;
#endif

    // set reference picture for alt-refs
    context_ptr->me_context_ptr->alt_ref_reference_ptr =
        (EbPaReferenceObject *)
            picture_control_set_ptr_frame->pa_reference_picture_wrapper_ptr->object_ptr;
    context_ptr->me_context_ptr->me_type = ME_MCTF;

    // set the buffers with the original, quarter and sixteenth pixels version of the source frame
    EbPaReferenceObject *src_object = (EbPaReferenceObject *)picture_control_set_ptr_central
                                          ->pa_reference_picture_wrapper_ptr->object_ptr;
    EbPictureBufferDesc *padded_pic_ptr = src_object->input_padded_picture_ptr;
#if !OPT_ONE_BUFFER_DOWNSAMPLED
    SequenceControlSet * scs_ptr        = (SequenceControlSet *)
                                      picture_control_set_ptr_central->scs_wrapper_ptr->object_ptr;
#endif
    // Set 1/4 and 1/16 ME reference buffer(s); filtered or decimated
#if OPT_ONE_BUFFER_DOWNSAMPLED
    EbPictureBufferDesc *quarter_pic_ptr = src_object->quarter_downsampled_picture_ptr;
    EbPictureBufferDesc *sixteenth_pic_ptr = src_object->sixteenth_downsampled_picture_ptr;
#else
    EbPictureBufferDesc *quarter_pic_ptr = (scs_ptr->down_sampling_method_me_search ==
                                            ME_FILTERED_DOWNSAMPLED)
        ? src_object->quarter_filtered_picture_ptr
        : src_object->quarter_decimated_picture_ptr;

    EbPictureBufferDesc *sixteenth_pic_ptr = (scs_ptr->down_sampling_method_me_search ==
                                              ME_FILTERED_DOWNSAMPLED)
        ? src_object->sixteenth_filtered_picture_ptr
        : src_object->sixteenth_decimated_picture_ptr;
#endif
    // Parts from MotionEstimationKernel()
    uint32_t sb_origin_x = (uint32_t)(blk_col * BW);
    uint32_t sb_origin_y = (uint32_t)(blk_row * BH);
#if ! SS_OPT_TF2_ME_COPY
    uint32_t sb_width  = (input_picture_ptr_central->width - sb_origin_x) < BLOCK_SIZE_64
         ? input_picture_ptr_central->width - sb_origin_x
         : BLOCK_SIZE_64;
    uint32_t sb_height = (input_picture_ptr_central->height - sb_origin_y) < BLOCK_SIZE_64
        ? input_picture_ptr_central->height - sb_origin_y
        : BLOCK_SIZE_64;
#endif

    // Load the SB from the input to the intermediate SB buffer
    int buffer_index = (input_picture_ptr_central->origin_y + sb_origin_y) *
            input_picture_ptr_central->stride_y +
        input_picture_ptr_central->origin_x + sb_origin_x;
    // set search method
    context_ptr->me_context_ptr->hme_search_method = FULL_SAD_SEARCH;

    // set Lambda
    context_ptr->me_context_ptr->lambda =
        lambda_mode_decision_ra_sad[picture_control_set_ptr_central->picture_qp];


#if  !OPT_ME_RES_SAD_LOOP
    // populate src block buffers: sb_buffer, quarter_sb_buffer and sixteenth_sb_buffer
    for (sb_row = 0; sb_row < BLOCK_SIZE_64; sb_row++) {
        svt_memcpy((&(context_ptr->me_context_ptr->sb_buffer[sb_row * BLOCK_SIZE_64])),
                   (&(input_picture_ptr_central
                          ->buffer_y[buffer_index + sb_row * input_picture_ptr_central->stride_y])),
                   BLOCK_SIZE_64 * sizeof(uint8_t));
    }
#endif
#ifdef ARCH_X86_64
    {
        uint8_t *src_ptr = &(padded_pic_ptr->buffer_y[buffer_index]);

#if SS_OPT_TF2_ME_COPY
        uint32_t sb_height = (input_picture_ptr_central->height - sb_origin_y) < BLOCK_SIZE_64
            ? input_picture_ptr_central->height - sb_origin_y
            : BLOCK_SIZE_64;
#endif
        //_MM_HINT_T0     //_MM_HINT_T1    //_MM_HINT_T2    //_MM_HINT_NTA
        uint32_t i;
        for (i = 0; i < sb_height; i++) {
            char const *p = (char const *)(src_ptr + i * padded_pic_ptr->stride_y);

            _mm_prefetch(p, _MM_HINT_T2);
        }
    }
#endif

    context_ptr->me_context_ptr->sb_src_ptr    = &(padded_pic_ptr->buffer_y[buffer_index]);
    context_ptr->me_context_ptr->sb_src_stride = padded_pic_ptr->stride_y;

    // Load the 1/4 decimated SB from the 1/4 decimated input to the 1/4 intermediate SB buffer
    buffer_index = (quarter_pic_ptr->origin_y + (sb_origin_y >> ss_y)) * quarter_pic_ptr->stride_y +
        quarter_pic_ptr->origin_x + (sb_origin_x >> ss_x);

#if SS_OPT_TF2_ME_COPY
    context_ptr->me_context_ptr->quarter_sb_buffer = &quarter_pic_ptr->buffer_y[buffer_index];
    context_ptr->me_context_ptr->quarter_sb_buffer_stride = quarter_pic_ptr->stride_y;
#else
    for (sb_row = 0; sb_row < (sb_height >> ss_y); sb_row++) {
        svt_memcpy(
            (&(context_ptr->me_context_ptr
                   ->quarter_sb_buffer[sb_row *
                                       context_ptr->me_context_ptr->quarter_sb_buffer_stride])),
            (&(quarter_pic_ptr->buffer_y[buffer_index + sb_row * quarter_pic_ptr->stride_y])),
            (sb_width >> ss_x) * sizeof(uint8_t));
    }
#endif

    // Load the 1/16 decimated SB from the 1/16 decimated input to the 1/16 intermediate SB buffer
    buffer_index = (sixteenth_pic_ptr->origin_y + (sb_origin_y >> 2)) *
            sixteenth_pic_ptr->stride_y +
        sixteenth_pic_ptr->origin_x + (sb_origin_x >> 2);

#if SS_OPT_TF2_ME_COPY
    context_ptr->me_context_ptr->sixteenth_sb_buffer = &sixteenth_pic_ptr->buffer_y[buffer_index];
    context_ptr->me_context_ptr->sixteenth_sb_buffer_stride = sixteenth_pic_ptr->stride_y;
#else

    {
        uint8_t *frame_ptr = &(sixteenth_pic_ptr->buffer_y[buffer_index]);
        uint8_t *local_ptr = context_ptr->me_context_ptr->sixteenth_sb_buffer;

        if (context_ptr->me_context_ptr->hme_search_method == FULL_SAD_SEARCH) {
            for (sb_row = 0; sb_row < (sb_height >> 2); sb_row += 1) {
                svt_memcpy(local_ptr, frame_ptr, (sb_width >> 2) * sizeof(uint8_t));
                local_ptr += 16;
                frame_ptr += sixteenth_pic_ptr->stride_y;
            }
        } else {
            for (sb_row = 0; sb_row < (sb_height >> 2); sb_row += 2) {
                svt_memcpy(local_ptr, frame_ptr, (sb_width >> 2) * sizeof(uint8_t));
                local_ptr += 16;
                frame_ptr += sixteenth_pic_ptr->stride_y << 1;
            }
        }
    }
#endif
}

static void create_me_context_and_picture_control_inl(
    MotionEstimationContext_t *context_ptr, PictureParentControlSet *picture_control_set_ptr_frame,
    PictureParentControlSet *picture_control_set_ptr_central,
    EbPictureBufferDesc *input_picture_ptr_central, int blk_row, int blk_col, uint32_t ss_x,
    uint32_t ss_y) {
    // set reference picture for alt-refs
    context_ptr->me_context_ptr->me_ds_ref_array[0][0] = picture_control_set_ptr_frame->ds_pics;
    context_ptr->me_context_ptr->me_type               = ME_MCTF;

    // set the buffers with the original, quarter and sixteenth pixels version of the source frame
    EbDownScaledObject *src_ds_object = (EbDownScaledObject *)picture_control_set_ptr_central
                                            ->down_scaled_picture_wrapper_ptr->object_ptr;

    // Set 1/4 and 1/16 ME reference buffer(s); filtered or decimated
    EbPictureBufferDesc *quarter_pic_ptr = src_ds_object->quarter_picture_ptr;

    EbPictureBufferDesc *sixteenth_pic_ptr = src_ds_object->sixteenth_picture_ptr;

    // Parts from MotionEstimationKernel()
    uint32_t sb_origin_x = (uint32_t)(blk_col * BW);
    uint32_t sb_origin_y = (uint32_t)(blk_row * BH);

    uint32_t sb_width  = (input_picture_ptr_central->width - sb_origin_x) < BLOCK_SIZE_64
         ? input_picture_ptr_central->width - sb_origin_x
         : BLOCK_SIZE_64;
    uint32_t sb_height = (input_picture_ptr_central->height - sb_origin_y) < BLOCK_SIZE_64
        ? input_picture_ptr_central->height - sb_origin_y
        : BLOCK_SIZE_64;
    // Load the SB from the input to the intermediate SB buffer
    int buffer_index = (input_picture_ptr_central->origin_y + sb_origin_y) *
            input_picture_ptr_central->stride_y +
        input_picture_ptr_central->origin_x + sb_origin_x;

    // set search method
    context_ptr->me_context_ptr->hme_search_method = FULL_SAD_SEARCH;

    // set Lambda
    context_ptr->me_context_ptr->lambda =
        lambda_mode_decision_ra_sad[picture_control_set_ptr_central->picture_qp];

#ifdef ARCH_X86_64
    {
        uint8_t *src_ptr = &(input_picture_ptr_central->buffer_y[buffer_index]);

        //_MM_HINT_T0     //_MM_HINT_T1    //_MM_HINT_T2    //_MM_HINT_NTA
        uint32_t i;
        for (i = 0; i < sb_height; i++) {
            char const *p = (char const *)(src_ptr + i * input_picture_ptr_central->stride_y);
            _mm_prefetch(p, _MM_HINT_T2);
        }
    }
#endif
    context_ptr->me_context_ptr->sb_src_ptr = &(input_picture_ptr_central->buffer_y[buffer_index]);
    context_ptr->me_context_ptr->sb_src_stride = input_picture_ptr_central->stride_y;

    // Load the 1/4 decimated SB from the 1/4 decimated input to the 1/4 intermediate SB buffer
    buffer_index = (quarter_pic_ptr->origin_y + (sb_origin_y >> ss_y)) * quarter_pic_ptr->stride_y +
        quarter_pic_ptr->origin_x + (sb_origin_x >> ss_x);

    for (uint32_t sb_row = 0; sb_row < (sb_height >> ss_y); sb_row++) {
        EB_MEMCPY((&(context_ptr->me_context_ptr->quarter_sb_buffer
                         [sb_row * context_ptr->me_context_ptr->quarter_sb_buffer_stride])),
                  (&(quarter_pic_ptr->buffer_y[buffer_index + sb_row * quarter_pic_ptr->stride_y])),
                  (sb_width >> ss_x) * sizeof(uint8_t));
    }

    // Load the 1/16 decimated SB from the 1/16 decimated input to the 1/16 intermediate SB buffer
    buffer_index = (sixteenth_pic_ptr->origin_y + (sb_origin_y >> 2)) *
            sixteenth_pic_ptr->stride_y +
        sixteenth_pic_ptr->origin_x + (sb_origin_x >> 2);

    {
        uint8_t *frame_ptr = &(sixteenth_pic_ptr->buffer_y[buffer_index]);
        uint8_t *local_ptr = context_ptr->me_context_ptr->sixteenth_sb_buffer;

        if (context_ptr->me_context_ptr->hme_search_method == FULL_SAD_SEARCH) {
            for (uint32_t sb_row = 0; sb_row < (sb_height >> 2); sb_row += 1) {
                EB_MEMCPY(local_ptr, frame_ptr, (sb_width >> 2) * sizeof(uint8_t));
                local_ptr += 16;
                frame_ptr += sixteenth_pic_ptr->stride_y;
            }
        } else {
            for (uint32_t sb_row = 0; sb_row < (sb_height >> 2); sb_row += 2) {
                EB_MEMCPY(local_ptr, frame_ptr, (sb_width >> 2) * sizeof(uint8_t));
                local_ptr += 16;
                frame_ptr += sixteenth_pic_ptr->stride_y << 1;
            }
        }
    }
}
static INLINE void calculate_squared_errors(const uint8_t *s, int s_stride, const uint8_t *p,
                                            int p_stride, uint16_t *diff_sse, unsigned int w,
                                            unsigned int h) {
    int          idx = 0;
    unsigned int i, j;

    for (i = 0; i < h; i++) {
        for (j = 0; j < w; j++) {
            const int16_t diff = s[i * s_stride + j] - p[i * p_stride + j];
            diff_sse[idx]      = (uint16_t)(diff * diff);
            idx++;
        }
    }
}

static INLINE void calculate_squared_errors_highbd(const uint16_t *s, int s_stride,
                                                   const uint16_t *p, int p_stride,
                                                   uint32_t *diff_sse, unsigned int w,
                                                   unsigned int h) {
    int          idx = 0;
    unsigned int i, j;

    for (i = 0; i < h; i++) {
        for (j = 0; j < w; j++) {
            const int32_t diff = s[i * s_stride + j] - p[i * p_stride + j];
            diff_sse[idx]      = (uint32_t)(diff * diff);
            idx++;
        }
    }
}

#if TUNE_REDESIGN_TF_CTRLS
// Apply filtering to the central picture
static void apply_filtering_central(MeContext *context_ptr, EbPictureBufferDesc *input_picture_ptr_central,EbByte *src, uint32_t **accum,
    uint16_t **count, uint16_t blk_width, uint16_t blk_height,
    uint32_t ss_x, uint32_t ss_y) {
    uint16_t blk_height_y = blk_height;
    uint16_t blk_width_y = blk_width;
    uint16_t blk_height_ch = blk_height >> ss_y;
    uint16_t blk_width_ch = blk_width >> ss_x;
    uint16_t src_stride_y = input_picture_ptr_central->stride_y;
    uint16_t src_stride_ch = src_stride_y >> ss_x;

    const int modifier = TF_PLANEWISE_FILTER_WEIGHT_SCALE;

    // Luma
    for (uint16_t k = 0, i = 0; i < blk_height_y; i++) {
        for (uint16_t j = 0; j < blk_width_y; j++) {
#if   SS_OPT_TF2_ME_COPY
            accum[C_Y][k] = modifier * src[C_Y][i * src_stride_y + j];
            count[C_Y][k] = modifier;
#else
            accum[C_Y][k] += modifier * src[C_Y][i * src_stride_y + j];
            count[C_Y][k] += modifier;
#endif
            ++k;
        }
    }

    // Chroma
    if (context_ptr->tf_chroma)
        for (uint16_t k = 0, i = 0; i < blk_height_ch; i++) {
            for (uint16_t j = 0; j < blk_width_ch; j++) {
#if   SS_OPT_TF2_ME_COPY
                accum[C_U][k] = modifier * src[C_U][i * src_stride_ch + j];
                count[C_U][k] = modifier;

                accum[C_V][k] = modifier * src[C_V][i * src_stride_ch + j];
                count[C_V][k] = modifier;
#else
                accum[C_U][k] += modifier * src[C_U][i * src_stride_ch + j];
                count[C_U][k] += modifier;

                accum[C_V][k] += modifier * src[C_V][i * src_stride_ch + j];
                count[C_V][k] += modifier;
#endif
                ++k;
            }
        }
}

// Apply filtering to the central picture
static void apply_filtering_central_highbd(MeContext *context_ptr, EbPictureBufferDesc *input_picture_ptr_central,uint16_t **src_16bit,
    uint32_t **accum, uint16_t **count, uint16_t blk_width,
    uint16_t blk_height, uint32_t ss_x, uint32_t ss_y) {
    uint16_t  blk_height_y = blk_height;
    uint16_t  blk_width_y = blk_width;
    uint16_t  blk_height_ch = blk_height >> ss_y;
    uint16_t  blk_width_ch = blk_width >> ss_x;
    uint16_t  src_stride_y = input_picture_ptr_central->stride_y;
    uint16_t  src_stride_ch = src_stride_y >> ss_x;

    const int modifier = TF_PLANEWISE_FILTER_WEIGHT_SCALE;

    // Luma
    for (uint16_t k = 0, i = 0; i < blk_height_y; i++) {
        for (uint16_t j = 0; j < blk_width_y; j++) {
#if   SS_OPT_TF2_ME_COPY
            accum[C_Y][k] = modifier * src_16bit[C_Y][i * src_stride_y + j];
            count[C_Y][k] = modifier;
#else
            accum[C_Y][k] += modifier * src_16bit[C_Y][i * src_stride_y + j];
            count[C_Y][k] += modifier;
#endif
            ++k;
        }
    }

    // Chroma
    if (context_ptr->tf_chroma)
        for (uint16_t k = 0, i = 0; i < blk_height_ch; i++) {
            for (uint16_t j = 0; j < blk_width_ch; j++) {
#if   SS_OPT_TF2_ME_COPY
                accum[C_U][k] = modifier * src_16bit[C_U][i * src_stride_ch + j];
                count[C_U][k] = modifier;

                accum[C_V][k] = modifier * src_16bit[C_V][i * src_stride_ch + j];
                count[C_V][k] = modifier;
#else
                accum[C_U][k] += modifier * src_16bit[C_U][i * src_stride_ch + j];
                count[C_U][k] += modifier;

                accum[C_V][k] += modifier * src_16bit[C_V][i * src_stride_ch + j];
                count[C_V][k] += modifier;
#endif
                ++k;
            }
        }
}
#else
// Apply filtering to the central picture
static void apply_filtering_central(MeContext *context_ptr, EbByte *pred, uint32_t **accum,
                                    uint16_t **count, uint16_t blk_width, uint16_t blk_height,
                                    uint32_t ss_x, uint32_t ss_y, int use_planewise_strategy) {
    uint16_t blk_height_y  = blk_height;
    uint16_t blk_width_y   = blk_width;
    uint16_t blk_height_ch = blk_height >> ss_y;
    uint16_t blk_width_ch  = blk_width >> ss_x;
    uint16_t blk_stride_y  = blk_width;
    uint16_t blk_stride_ch = blk_width >> ss_x;

    const int modifier = use_planewise_strategy ? TF_PLANEWISE_FILTER_WEIGHT_SCALE
                                                : INIT_WEIGHT * WEIGHT_MULTIPLIER;
    // Luma
    for (uint16_t k = 0, i = 0; i < blk_height_y; i++) {
        for (uint16_t j = 0; j < blk_width_y; j++) {
            accum[C_Y][k] += modifier * pred[C_Y][i * blk_stride_y + j];
            count[C_Y][k] += modifier;
            ++k;
        }
    }

    // Chroma
    if (context_ptr->tf_chroma)
        for (uint16_t k = 0, i = 0; i < blk_height_ch; i++) {
            for (uint16_t j = 0; j < blk_width_ch; j++) {
                accum[C_U][k] += modifier * pred[C_U][i * blk_stride_ch + j];
                count[C_U][k] += modifier;

                accum[C_V][k] += modifier * pred[C_V][i * blk_stride_ch + j];
                count[C_V][k] += modifier;
                ++k;
            }
        }
}

// Apply filtering to the central picture
static void apply_filtering_central_highbd(MeContext *context_ptr, uint16_t **pred_16bit,
                                           uint32_t **accum, uint16_t **count, uint16_t blk_width,
                                           uint16_t blk_height, uint32_t ss_x, uint32_t ss_y,
                                           int use_planewise_strategy) {
    uint16_t  blk_height_y  = blk_height;
    uint16_t  blk_width_y   = blk_width;
    uint16_t  blk_height_ch = blk_height >> ss_y;
    uint16_t  blk_width_ch  = blk_width >> ss_x;
    uint16_t  blk_stride_y  = blk_width;
    uint16_t  blk_stride_ch = blk_width >> ss_x;
    const int modifier      = use_planewise_strategy ? TF_PLANEWISE_FILTER_WEIGHT_SCALE
                                                     : INIT_WEIGHT * WEIGHT_MULTIPLIER;
    // Luma
    for (uint16_t k = 0, i = 0; i < blk_height_y; i++) {
        for (uint16_t j = 0; j < blk_width_y; j++) {
            accum[C_Y][k] += modifier * pred_16bit[C_Y][i * blk_stride_y + j];
            count[C_Y][k] += modifier;
            ++k;
        }
    }

    // Chroma
    if (context_ptr->tf_chroma)
        for (uint16_t k = 0, i = 0; i < blk_height_ch; i++) {
            for (uint16_t j = 0; j < blk_width_ch; j++) {
                accum[C_U][k] += modifier * pred_16bit[C_U][i * blk_stride_ch + j];
                count[C_U][k] += modifier;

                accum[C_V][k] += modifier * pred_16bit[C_V][i * blk_stride_ch + j];
                count[C_V][k] += modifier;
                ++k;
            }
        }
}
#endif
/***************************************************************************************************
* Applies temporal filter plane by plane.
* Inputs:
*   y_src, u_src, v_src : Pointers to the frame to be filtered, which is used as
*                    reference to compute squared differece from the predictor.
*   block_width: Width of the block.
*   block_height: Height of the block
*   noise_levels: Pointer to the noise levels of the to-filter frame, estimated
*                 with each plane (in Y, U, V order).
*   y_pre, r_pre, v_pre: Pointers to the well-built predictors.
*   accum: Pointer to the pixel-wise accumulator for filtering.
*   count: Pointer to the pixel-wise counter fot filtering.
* Returns:
*   Nothing will be returned. But the content to which `accum` and `pred`
*   point will be modified.
***************************************************************************************************/
void svt_av1_apply_temporal_filter_planewise_c(
    struct MeContext *context_ptr, const uint8_t *y_src, int y_src_stride, const uint8_t *y_pre,
    int y_pre_stride, const uint8_t *u_src, const uint8_t *v_src, int uv_src_stride,
    const uint8_t *u_pre, const uint8_t *v_pre, int uv_pre_stride, unsigned int block_width,
    unsigned int block_height, int ss_x, int ss_y, const double *noise_levels,
    const int decay_control, uint32_t *y_accum, uint16_t *y_count, uint32_t *u_accum,
    uint16_t *u_count, uint32_t *v_accum, uint16_t *v_count) {
    unsigned int       i, j, k, m;
    int                idx, idy;
    uint64_t           sum_square_diff;
    const unsigned int uv_block_width  = block_width >> ss_x;
    const unsigned int uv_block_height = block_height >> ss_y;
    DECLARE_ALIGNED(16, uint16_t, y_diff_se[BLK_PELS]);
    DECLARE_ALIGNED(16, uint16_t, u_diff_se[BLK_PELS]);
    DECLARE_ALIGNED(16, uint16_t, v_diff_se[BLK_PELS]);

    memset(y_diff_se, 0, BLK_PELS * sizeof(uint16_t));
    memset(u_diff_se, 0, BLK_PELS * sizeof(uint16_t));
    memset(v_diff_se, 0, BLK_PELS * sizeof(uint16_t));

    // Calculate squared differences for each pixel of the block (pred-orig)
    calculate_squared_errors(
        y_src, y_src_stride, y_pre, y_pre_stride, y_diff_se, block_width, block_height);
    if (context_ptr->tf_chroma) {
        calculate_squared_errors(
            u_src, uv_src_stride, u_pre, uv_pre_stride, u_diff_se, uv_block_width, uv_block_height);
        calculate_squared_errors(
            v_src, uv_src_stride, v_pre, uv_pre_stride, v_diff_se, uv_block_width, uv_block_height);
    }

    // Get window size for pixel-wise filtering.
    assert(TF_PLANEWISE_FILTER_WINDOW_LENGTH % 2 == 1);
    const int half_window = TF_PLANEWISE_FILTER_WINDOW_LENGTH >> 1;
    for (i = 0; i < block_height; i++) {
        for (j = 0; j < block_width; j++) {
            const int pixel_value = y_pre[i * y_pre_stride + j];
            // non-local mean approach
            int num_ref_pixels = 0;

            const int uv_r  = i >> ss_y;
            const int uv_c  = j >> ss_x;
            sum_square_diff = 0;
            for (idy = -half_window; idy <= half_window; ++idy) {
                for (idx = -half_window; idx <= half_window; ++idx) {
                    const int row = CLIP((int)i + idy, 0, (int)block_height - 1);
                    const int col = CLIP((int)j + idx, 0, (int)block_width - 1);
                    sum_square_diff += y_diff_se[row * (int)block_width + col];
                    ++num_ref_pixels;
                }
            }
            // Combine window error and block error, and normalize it.
            double window_error = (double)sum_square_diff / num_ref_pixels;

            const int subblock_idx = (i >= block_height / 2) * 2 + (j >= block_width / 2);
            double    block_error;
            int       idx_32x32 = context_ptr->tf_block_col + context_ptr->tf_block_row * 2;
            if (context_ptr->tf_32x32_block_split_flag[idx_32x32])
                // 16x16
                block_error =
                    (double)context_ptr->tf_16x16_block_error[idx_32x32 * 4 + subblock_idx] / 256;
            else
                //32x32
                block_error = (double)context_ptr->tf_32x32_block_error[idx_32x32] / 1024;

            double combined_error = (TF_WINDOW_BLOCK_BALANCE_WEIGHT * window_error + block_error) /
                (TF_WINDOW_BLOCK_BALANCE_WEIGHT + 1);

            // Decay factors for non-local mean approach.
            // Larger noise -> larger filtering weight.
            double n_decay = (double)decay_control * (0.7 + log1p(noise_levels[0]));
            // Smaller q -> smaller filtering weight. WIP
            const double q_decay = 1;
            //  CLIP(pow((double)q_factor / TF_Q_DECAY_THRESHOLD, 2), 1e-5, 1);
            // Smaller strength -> smaller filtering weight. WIP
            const double s_decay = 1;
            // CLIP(
            //    pow((double)filter_strength / TF_STRENGTH_THRESHOLD, 2), 1e-5, 1);
            // Larger motion vector -> smaller filtering weight.
            MV mv;
            if (context_ptr->tf_32x32_block_split_flag[idx_32x32]) {
                // 16x16
                mv.col = context_ptr->tf_16x16_mv_x[idx_32x32 * 4 + subblock_idx];
                mv.row = context_ptr->tf_16x16_mv_y[idx_32x32 * 4 + subblock_idx];
            } else {
                //32x32
                mv.col = context_ptr->tf_32x32_mv_x[idx_32x32];
                mv.row = context_ptr->tf_32x32_mv_y[idx_32x32];
            }
            const float  distance           = sqrtf(powf(mv.row, 2) + powf(mv.col, 2));
            const double distance_threshold = (double)AOMMAX(
                context_ptr->min_frame_size * TF_SEARCH_DISTANCE_THRESHOLD, 1);
            const double d_factor = AOMMAX(distance / distance_threshold, 1);

            // Compute filter weight.
            double scaled_diff = AOMMIN(
                combined_error * d_factor / (2 * n_decay * n_decay) / q_decay / s_decay, 7);
            int adjusted_weight = (int)(expf((float)(-scaled_diff)) * TF_WEIGHT_SCALE);
            k                   = i * y_pre_stride + j;
            y_count[k] += adjusted_weight;
            y_accum[k] += adjusted_weight * pixel_value;

            // Process chroma component
            if (context_ptr->tf_chroma)
                if (!(i & ss_y) && !(j & ss_x)) {
                    const int u_pixel_value = u_pre[uv_r * uv_pre_stride + uv_c];
                    const int v_pixel_value = v_pre[uv_r * uv_pre_stride + uv_c];
                    // non-local mean approach
                    num_ref_pixels             = 0;
                    uint64_t u_sum_square_diff = 0, v_sum_square_diff = 0;
                    sum_square_diff = 0;
                    // Filter U-plane and V-plane using Y-plane. This is because motion
                    // search is only done on Y-plane, so the information from Y-plane will
                    // be more accurate.
                    for (idy = 0; idy < (1 << ss_y); ++idy) {
                        for (idx = 0; idx < (1 << ss_x); ++idx) {
                            const int row = (int)i + idy;
                            const int col = (int)j + idx;
                            sum_square_diff += y_diff_se[row * (int)block_width + col];
                            ++num_ref_pixels;
                        }
                    }
                    u_sum_square_diff = sum_square_diff;
                    v_sum_square_diff = sum_square_diff;

                    for (idy = -half_window; idy <= half_window; ++idy) {
                        for (idx = -half_window; idx <= half_window; ++idx) {
                            const int row = CLIP((int)uv_r + idy, 0, (int)uv_block_height - 1);
                            const int col = CLIP((int)uv_c + idx, 0, (int)uv_block_width - 1);
                            u_sum_square_diff += u_diff_se[row * uv_block_width + col];
                            v_sum_square_diff += v_diff_se[row * uv_block_width + col];
                            ++num_ref_pixels;
                        }
                    }

                    m = (i >> ss_y) * uv_pre_stride + (j >> ss_x);
                    // Combine window error and block error, and normalize it.
                    window_error   = (double)u_sum_square_diff / num_ref_pixels;
                    combined_error = (TF_WINDOW_BLOCK_BALANCE_WEIGHT * window_error + block_error) /
                        (TF_WINDOW_BLOCK_BALANCE_WEIGHT + 1);

                    // Decay factors for non-local mean approach.
                    // Larger noise -> larger filtering weight.
                    n_decay = (double)decay_control * (0.7 + log1p(noise_levels[1]));
                    // Compute filter weight.
                    scaled_diff = AOMMIN(
                        combined_error * d_factor / (2 * n_decay * n_decay) / q_decay / s_decay, 7);
                    adjusted_weight = (int)(expf((float)(-scaled_diff)) * TF_WEIGHT_SCALE);
                    u_count[m] += adjusted_weight;
                    u_accum[m] += adjusted_weight * u_pixel_value;

                    // Combine window error and block error, and normalize it.
                    window_error   = (double)v_sum_square_diff / num_ref_pixels;
                    combined_error = (TF_WINDOW_BLOCK_BALANCE_WEIGHT * window_error + block_error) /
                        (TF_WINDOW_BLOCK_BALANCE_WEIGHT + 1);

                    // Decay factors for non-local mean approach.
                    // Larger noise -> larger filtering weight.
                    n_decay = (double)decay_control * (0.7 + log1p(noise_levels[2]));

                    // Compute filter weight.
                    scaled_diff = AOMMIN(
                        combined_error * d_factor / (2 * n_decay * n_decay) / q_decay / s_decay, 7);
                    adjusted_weight = (int)(expf((float)(-scaled_diff)) * TF_WEIGHT_SCALE);
                    v_count[m] += adjusted_weight;
                    v_accum[m] += adjusted_weight * v_pixel_value;
                }
        }
    }
}

/***************************************************************************************************
* Applies temporal filter plane by plane for hbd
* Inputs:
*   y_src, u_src, v_src : Pointers to the frame to be filtered, which is used as
*                    reference to compute squared differece from the predictor.
*   block_width: Width of the block.
*   block_height: Height of the block
*   noise_levels: Pointer to the noise levels of the to-filter frame, estimated
*                 with each plane (in Y, U, V order).
*   y_pre, r_pre, v_pre: Pointers to the well-built predictors.
*   accum: Pointer to the pixel-wise accumulator for filtering.
*   count: Pointer to the pixel-wise counter fot filtering.
* Returns:
*   Nothing will be returned. But the content to which `accum` and `pred`
*   point will be modified.
***************************************************************************************************/
void svt_av1_apply_temporal_filter_planewise_hbd_c(
    struct MeContext *context_ptr, const uint16_t *y_src, int y_src_stride, const uint16_t *y_pre,
    int y_pre_stride, const uint16_t *u_src, const uint16_t *v_src, int uv_src_stride,
    const uint16_t *u_pre, const uint16_t *v_pre, int uv_pre_stride, unsigned int block_width,
    unsigned int block_height, int ss_x, int ss_y, const double *noise_levels,
    const int decay_control, uint32_t *y_accum, uint16_t *y_count, uint32_t *u_accum,
    uint16_t *u_count, uint32_t *v_accum, uint16_t *v_count, uint32_t encoder_bit_depth) {
    unsigned int       i, j, k, m;
    int                idx, idy;
    uint64_t           sum_square_diff;
    const unsigned int uv_block_width  = block_width >> ss_x;
    const unsigned int uv_block_height = block_height >> ss_y;
    DECLARE_ALIGNED(16, uint32_t, y_diff_se[BLK_PELS]);
    DECLARE_ALIGNED(16, uint32_t, u_diff_se[BLK_PELS]);
    DECLARE_ALIGNED(16, uint32_t, v_diff_se[BLK_PELS]);

    memset(y_diff_se, 0, BLK_PELS * sizeof(uint32_t));
    memset(u_diff_se, 0, BLK_PELS * sizeof(uint32_t));
    memset(v_diff_se, 0, BLK_PELS * sizeof(uint32_t));

    // Calculate squared differences for each pixel of the block (pred-orig)
    calculate_squared_errors_highbd(
        y_src, y_src_stride, y_pre, y_pre_stride, y_diff_se, block_width, block_height);
    if (context_ptr->tf_chroma) {
        calculate_squared_errors_highbd(
            u_src, uv_src_stride, u_pre, uv_pre_stride, u_diff_se, uv_block_width, uv_block_height);
        calculate_squared_errors_highbd(
            v_src, uv_src_stride, v_pre, uv_pre_stride, v_diff_se, uv_block_width, uv_block_height);
    }
    // Get window size for pixel-wise filtering.
    assert(TF_PLANEWISE_FILTER_WINDOW_LENGTH % 2 == 1);
    const int half_window = TF_PLANEWISE_FILTER_WINDOW_LENGTH >> 1;
    for (i = 0; i < block_height; i++) {
        for (j = 0; j < block_width; j++) {
            const int pixel_value = y_pre[i * y_pre_stride + j];
            // non-local mean approach
            int num_ref_pixels = 0;

            const int uv_r  = i >> ss_y;
            const int uv_c  = j >> ss_x;
            sum_square_diff = 0;
            for (idy = -half_window; idy <= half_window; ++idy) {
                for (idx = -half_window; idx <= half_window; ++idx) {
                    const int row = CLIP((int)i + idy, 0, (int)block_height - 1);
                    const int col = CLIP((int)j + idx, 0, (int)block_width - 1);
                    sum_square_diff += y_diff_se[row * (int)block_width + col];
                    ++num_ref_pixels;
                }
            }
            // Combine window error and block error, and normalize it.
            // Scale down the difference for high bit depth input.
            sum_square_diff >>= ((encoder_bit_depth - 8) * 2);
            double window_error = (double)sum_square_diff / num_ref_pixels;

            const int subblock_idx = (i >= block_height / 2) * 2 + (j >= block_width / 2);
            double    block_error;
            int       idx_32x32 = context_ptr->tf_block_col + context_ptr->tf_block_row * 2;
            if (context_ptr->tf_32x32_block_split_flag[idx_32x32])
                // 16x16
                // Scale down the difference for high bit depth input.
                block_error =
                    (double)(context_ptr->tf_16x16_block_error[idx_32x32 * 4 + subblock_idx] >> 4) /
                    256;
            else
                //32x32
                // Scale down the difference for high bit depth input.
                block_error = (double)(context_ptr->tf_32x32_block_error[idx_32x32] >> 4) / 1024;

            double combined_error = (TF_WINDOW_BLOCK_BALANCE_WEIGHT * window_error + block_error) /
                (TF_WINDOW_BLOCK_BALANCE_WEIGHT + 1);

            // Decay factors for non-local mean approach.
            // Larger noise -> larger filtering weight.
            double n_decay = (double)decay_control * (0.7 + log1p(noise_levels[0]));
            // Smaller q -> smaller filtering weight. WIP
            const double q_decay = 1;
            //  CLIP(pow((double)q_factor / TF_Q_DECAY_THRESHOLD, 2), 1e-5, 1);
            // Smaller strength -> smaller filtering weight. WIP
            const double s_decay = 1;
            // CLIP(
            //    pow((double)filter_strength / TF_STRENGTH_THRESHOLD, 2), 1e-5, 1);
            // Larger motion vector -> smaller filtering weight.
            MV mv;
            if (context_ptr->tf_32x32_block_split_flag[idx_32x32]) {
                // 16x16
                mv.col = context_ptr->tf_16x16_mv_x[idx_32x32 * 4 + subblock_idx];
                mv.row = context_ptr->tf_16x16_mv_y[idx_32x32 * 4 + subblock_idx];
            } else {
                //32x32
                mv.col = context_ptr->tf_32x32_mv_x[idx_32x32];
                mv.row = context_ptr->tf_32x32_mv_y[idx_32x32];
            }
            const float  distance           = sqrtf(powf(mv.row, 2) + powf(mv.col, 2));
            const double distance_threshold = (double)AOMMAX(
                context_ptr->min_frame_size * TF_SEARCH_DISTANCE_THRESHOLD, 1);
            const double d_factor = AOMMAX(distance / distance_threshold, 1);

            // Compute filter weight.
            double scaled_diff = AOMMIN(
                combined_error * d_factor / (2 * n_decay * n_decay) / q_decay / s_decay, 7);
            int adjusted_weight = (int)(expf((float)-scaled_diff) * TF_WEIGHT_SCALE);
            k                   = i * y_pre_stride + j;
            y_count[k] += adjusted_weight;
            y_accum[k] += adjusted_weight * pixel_value;

            // Process chroma component
            if (context_ptr->tf_chroma)
                if (!(i & ss_y) && !(j & ss_x)) {
                    const int u_pixel_value = u_pre[uv_r * uv_pre_stride + uv_c];
                    const int v_pixel_value = v_pre[uv_r * uv_pre_stride + uv_c];
                    // non-local mean approach
                    num_ref_pixels             = 0;
                    uint64_t u_sum_square_diff = 0, v_sum_square_diff = 0;
                    sum_square_diff = 0;
                    // Filter U-plane and V-plane using Y-plane. This is because motion
                    // search is only done on Y-plane, so the information from Y-plane will
                    // be more accurate.
                    for (idy = 0; idy < (1 << ss_y); ++idy) {
                        for (idx = 0; idx < (1 << ss_x); ++idx) {
                            const int row = (int)i + idy;
                            const int col = (int)j + idx;
                            sum_square_diff += y_diff_se[row * (int)block_width + col];
                            ++num_ref_pixels;
                        }
                    }
                    u_sum_square_diff = sum_square_diff;
                    v_sum_square_diff = sum_square_diff;

                    for (idy = -half_window; idy <= half_window; ++idy) {
                        for (idx = -half_window; idx <= half_window; ++idx) {
                            const int row = CLIP((int)uv_r + idy, 0, (int)uv_block_height - 1);
                            const int col = CLIP((int)uv_c + idx, 0, (int)uv_block_width - 1);
                            u_sum_square_diff += u_diff_se[row * uv_block_width + col];
                            v_sum_square_diff += v_diff_se[row * uv_block_width + col];
                            ++num_ref_pixels;
                        }
                    }

                    m = (i >> ss_y) * uv_pre_stride + (j >> ss_x);
                    // Scale down the difference for high bit depth input.
                    u_sum_square_diff >>= ((encoder_bit_depth - 8) * 2);
                    v_sum_square_diff >>= ((encoder_bit_depth - 8) * 2);
                    // Combine window error and block error, and normalize it.
                    window_error   = (double)u_sum_square_diff / num_ref_pixels;
                    combined_error = (TF_WINDOW_BLOCK_BALANCE_WEIGHT * window_error + block_error) /
                        (TF_WINDOW_BLOCK_BALANCE_WEIGHT + 1);

                    // Decay factors for non-local mean approach.
                    // Larger noise -> larger filtering weight.
                    n_decay = (double)decay_control * (0.7 + log1p(noise_levels[1]));
                    // Compute filter weight.
                    scaled_diff = AOMMIN(
                        combined_error * d_factor / (2 * n_decay * n_decay) / q_decay / s_decay, 7);
                    adjusted_weight = (int)(expf((float)-scaled_diff) * TF_WEIGHT_SCALE);
                    u_count[m] += adjusted_weight;
                    u_accum[m] += adjusted_weight * u_pixel_value;

                    // Combine window error and block error, and normalize it.
                    window_error   = (double)v_sum_square_diff / num_ref_pixels;
                    combined_error = (TF_WINDOW_BLOCK_BALANCE_WEIGHT * window_error + block_error) /
                        (TF_WINDOW_BLOCK_BALANCE_WEIGHT + 1);

                    // Decay factors for non-local mean approach.
                    // Larger noise -> larger filtering weight.
                    n_decay = (double)decay_control * (0.7 + log1p(noise_levels[2]));

                    // Compute filter weight.
                    scaled_diff = AOMMIN(
                        combined_error * d_factor / (2 * n_decay * n_decay) / q_decay / s_decay, 7);
                    adjusted_weight = (int)(expf((float)-scaled_diff) * TF_WEIGHT_SCALE);
                    v_count[m] += adjusted_weight;
                    v_accum[m] += adjusted_weight * v_pixel_value;
                }
        }
    }
}
/***************************************************************************************************
* Applies temporal filter for each block plane by plane. Passes the right inputs
* for 8 bit  and HBD path
* Inputs:
*   src : Pointer to the 8 bit frame to be filtered, which is used as
*                    reference to compute squared differece from the predictor.
*   src_16bit : Pointer to the hbd frame to be filtered, which is used as
*                    reference to compute squared differece from the predictor.
*   block_width: Width of the block.
*   block_height: Height of the block.
*   noise_levels: Pointer to the noise levels of the to-filter frame, estimated
*                 with each plane (in Y, U, V order).
*   pred: Pointers to the well-built 8 bit predictors.
*   pred_16bit: Pointers to the well-built hbd predictors.
*   accum: Pointer to the pixel-wise accumulator for filtering.
*   count: Pointer to the pixel-wise counter fot filtering.
* Returns:
*   Nothing will be returned. But the content to which `accum` and `pred`
*   point will be modified.
***************************************************************************************************/
static void apply_filtering_block_plane_wise(
    MeContext *context_ptr, int block_row, int block_col, EbByte *src, uint16_t **src_16bit,
    EbByte *pred, uint16_t **pred_16bit, uint32_t **accum, uint16_t **count, uint32_t *stride,
    uint32_t *stride_pred, int block_width, int block_height, uint32_t ss_x, uint32_t ss_y,
    const double *noise_levels, const int decay_control, uint32_t encoder_bit_depth) {
    int blk_h               = block_height;
    int blk_w               = block_width;
    int offset_src_buffer_Y = block_row * blk_h * stride[C_Y] + block_col * blk_w;
    int offset_src_buffer_U = block_row * (blk_h >> ss_y) * stride[C_U] +
        block_col * (blk_w >> ss_x);
    int offset_src_buffer_V = block_row * (blk_h >> ss_y) * stride[C_V] +
        block_col * (blk_w >> ss_x);

    int offset_block_buffer_Y = block_row * blk_h * stride_pred[C_Y] + block_col * blk_w;
    int offset_block_buffer_U = block_row * (blk_h >> ss_y) * stride_pred[C_U] +
        block_col * (blk_w >> ss_x);
    int offset_block_buffer_V = block_row * (blk_h >> ss_y) * stride_pred[C_V] +
        block_col * (blk_w >> ss_x);

    uint32_t *accum_ptr[COLOR_CHANNELS];
    uint16_t *count_ptr[COLOR_CHANNELS];

    accum_ptr[C_Y] = accum[C_Y] + offset_block_buffer_Y;
    accum_ptr[C_U] = accum[C_U] + offset_block_buffer_U;
    accum_ptr[C_V] = accum[C_V] + offset_block_buffer_V;

    count_ptr[C_Y] = count[C_Y] + offset_block_buffer_Y;
    count_ptr[C_U] = count[C_U] + offset_block_buffer_U;
    count_ptr[C_V] = count[C_V] + offset_block_buffer_V;

    if (encoder_bit_depth == 8) {
        uint8_t *src_ptr[COLOR_CHANNELS] = {
            src[C_Y] + offset_src_buffer_Y,
            src[C_U] + offset_src_buffer_U,
            src[C_V] + offset_src_buffer_V,
        };

        uint8_t *pred_ptr[COLOR_CHANNELS] = {
            pred[C_Y] + offset_block_buffer_Y,
            pred[C_U] + offset_block_buffer_U,
            pred[C_V] + offset_block_buffer_V,
        };

        svt_av1_apply_temporal_filter_planewise(context_ptr,
                                                src_ptr[C_Y],
                                                stride[C_Y],
                                                pred_ptr[C_Y],
                                                stride_pred[C_Y],
                                                src_ptr[C_U],
                                                src_ptr[C_V],
                                                stride[C_U],
                                                pred_ptr[C_U],
                                                pred_ptr[C_V],
                                                stride_pred[C_U],
                                                (unsigned int)block_width,
                                                (unsigned int)block_height,
                                                ss_x,
                                                ss_y,
                                                noise_levels,
                                                decay_control,
                                                accum_ptr[C_Y],
                                                count_ptr[C_Y],
                                                accum_ptr[C_U],
                                                count_ptr[C_U],
                                                accum_ptr[C_V],
                                                count_ptr[C_V]);
    } else {
        uint16_t *src_ptr_16bit[COLOR_CHANNELS] = {
            src_16bit[C_Y] + offset_src_buffer_Y,
            src_16bit[C_U] + offset_src_buffer_U,
            src_16bit[C_V] + offset_src_buffer_V,
        };

        uint16_t *pred_ptr_16bit[COLOR_CHANNELS] = {
            pred_16bit[C_Y] + offset_block_buffer_Y,
            pred_16bit[C_U] + offset_block_buffer_U,
            pred_16bit[C_V] + offset_block_buffer_V,
        };

        // Apply the temporal filtering strategy
        // TODO(any): avx2 version should also support high bit-depth.
        svt_av1_apply_temporal_filter_planewise_hbd(context_ptr,
                                                    src_ptr_16bit[C_Y],
                                                    stride[C_Y],
                                                    pred_ptr_16bit[C_Y],
                                                    stride_pred[C_Y],
                                                    src_ptr_16bit[C_U],
                                                    src_ptr_16bit[C_V],
                                                    stride[C_U],
                                                    pred_ptr_16bit[C_U],
                                                    pred_ptr_16bit[C_V],
                                                    stride_pred[C_U],
                                                    (unsigned int)block_width,
                                                    (unsigned int)block_height,
                                                    ss_x,
                                                    ss_y,
                                                    noise_levels,
                                                    decay_control,
                                                    accum_ptr[C_Y],
                                                    count_ptr[C_Y],
                                                    accum_ptr[C_U],
                                                    count_ptr[C_U],
                                                    accum_ptr[C_V],
                                                    count_ptr[C_V],
                                                    encoder_bit_depth);
    }
}
uint32_t    get_mds_idx(uint32_t orgx, uint32_t orgy, uint32_t size, uint32_t use_128x128);
static void tf_16x16_sub_pel_search(PictureParentControlSet *pcs_ptr, MeContext *context_ptr,
                                    PictureParentControlSet *pcs_ref,
                                    EbPictureBufferDesc *pic_ptr_ref, EbByte *pred,
                                    uint16_t **pred_16bit, uint32_t *stride_pred, EbByte *src,
                                    uint16_t **src_16bit, uint32_t *stride_src,
                                    uint32_t sb_origin_x, uint32_t sb_origin_y, uint32_t ss_x,
                                    int encoder_bit_depth) {

#if FTR_SCALE_FACTOR
    SequenceControlSet *scs_ptr =
        (SequenceControlSet *)pcs_ptr->scs_wrapper_ptr->object_ptr;
#endif

    InterpFilters interp_filters = av1_make_interp_filters(EIGHTTAP_REGULAR, EIGHTTAP_REGULAR);

    EbBool is_highbd = (encoder_bit_depth == 8) ? (uint8_t)EB_FALSE : (uint8_t)EB_TRUE;

    BlkStruct   blk_ptr;
    MacroBlockD av1xd;
    blk_ptr.av1xd = &av1xd;
    MvUnit mv_unit;
    mv_unit.pred_direction = UNI_PRED_LIST_0;

    EbPictureBufferDesc reference_ptr;
    EbPictureBufferDesc prediction_ptr;

    UNUSED(ss_x);

    prediction_ptr.origin_x  = 0;
    prediction_ptr.origin_y  = 0;
    prediction_ptr.stride_y  = BW;
    prediction_ptr.stride_cb = (uint16_t)BW >> ss_x;
    prediction_ptr.stride_cr = (uint16_t)BW >> ss_x;

    if (!is_highbd) {
        assert(src[C_Y] != NULL);
        if (context_ptr->tf_chroma) {
            assert(src[C_U] != NULL);
            assert(src[C_V] != NULL);
        }
        prediction_ptr.buffer_y  = pred[C_Y];
        prediction_ptr.buffer_cb = pred[C_U];
        prediction_ptr.buffer_cr = pred[C_V];
    } else {
        assert(src_16bit[C_Y] != NULL);
        if (context_ptr->tf_chroma) {
            assert(src_16bit[C_U] != NULL);
            assert(src_16bit[C_V] != NULL);
        }
        prediction_ptr.buffer_y  = (uint8_t *)pred_16bit[C_Y];
        prediction_ptr.buffer_cb = (uint8_t *)pred_16bit[C_U];
        prediction_ptr.buffer_cr = (uint8_t *)pred_16bit[C_V];

        reference_ptr.buffer_y  = (uint8_t *)pcs_ref->altref_buffer_highbd[C_Y];
        reference_ptr.buffer_cb = (uint8_t *)pcs_ref->altref_buffer_highbd[C_U];
        reference_ptr.buffer_cr = (uint8_t *)pcs_ref->altref_buffer_highbd[C_V];
        reference_ptr.origin_x  = pic_ptr_ref->origin_x;
        reference_ptr.origin_y  = pic_ptr_ref->origin_y;
        reference_ptr.stride_y  = pic_ptr_ref->stride_y;
        reference_ptr.stride_cb = pic_ptr_ref->stride_cb;
        reference_ptr.stride_cr = pic_ptr_ref->stride_cr;
        reference_ptr.width     = pic_ptr_ref->width;
        reference_ptr.height    = pic_ptr_ref->height;
    }

    uint32_t bsize = 16;
#if TUNE_REDESIGN_TF_CTRLS
    uint32_t idx_32x32 = context_ptr->idx_32x32;
#else
    for (uint32_t idx_32x32 = 0; idx_32x32 < 4; idx_32x32++) {
#endif
#if TUNE_REDESIGN_TF_CTRLS
        context_ptr->tf_16x16_search_do[idx_32x32] =
            (context_ptr->tf_32x32_block_error[idx_32x32] < pcs_ptr->tf_ctrls.pred_error_32x32_th)
            ? 0
            : 1;
#else
        context_ptr->tf_16x16_search_do[idx_32x32] = (context_ptr->tf_32x32_block_error[idx_32x32] <
                                                      context_ptr->tf_block_32x32_16x16_th)
            ? 0
            : 1;
#endif
        if (context_ptr->tf_16x16_search_do[idx_32x32])
            for (uint32_t idx_16x16 = 0; idx_16x16 < 4; idx_16x16++) {
#if TUNE_REDESIGN_TF_CTRLS
                uint32_t pu_index = idx_32x32_to_idx_16x16[idx_32x32][idx_16x16];
#else
                uint32_t pu_index = index_16x16_from_subindexes[idx_32x32][idx_16x16];
#endif

                uint32_t idx_y          = subblock_xy_16x16[pu_index][0];
                uint32_t idx_x          = subblock_xy_16x16[pu_index][1];
                uint16_t local_origin_x = idx_x * bsize;
                uint16_t local_origin_y = idx_y * bsize;
                uint16_t pu_origin_x    = sb_origin_x + local_origin_x;
                uint16_t pu_origin_y    = sb_origin_y + local_origin_y;
                uint32_t mirow          = pu_origin_y >> MI_SIZE_LOG2;
                uint32_t micol          = pu_origin_x >> MI_SIZE_LOG2;
                blk_ptr.mds_idx         = get_mds_idx(
                    local_origin_x,
                    local_origin_y,
                    bsize,
                    pcs_ptr->scs_ptr->seq_header.sb_size == BLOCK_128X128);

                const int32_t bw                 = mi_size_wide[BLOCK_16X16];
                const int32_t bh                 = mi_size_high[BLOCK_16X16];
                blk_ptr.av1xd->mb_to_top_edge    = -(int32_t)((mirow * MI_SIZE) * 8);
                blk_ptr.av1xd->mb_to_bottom_edge = ((pcs_ptr->av1_cm->mi_rows - bw - mirow) *
                                                    MI_SIZE) *
                    8;
                blk_ptr.av1xd->mb_to_left_edge  = -(int32_t)((micol * MI_SIZE) * 8);
                blk_ptr.av1xd->mb_to_right_edge = ((pcs_ptr->av1_cm->mi_cols - bh - micol) *
                                                   MI_SIZE) *
                    8;

                uint32_t mv_index = tab16x16[pu_index];
                mv_unit.mv->x     = _MVXT(context_ptr->p_best_mv16x16[mv_index]);
                mv_unit.mv->y     = _MVYT(context_ptr->p_best_mv16x16[mv_index]);
                // AV1 MVs are always in 1/8th pel precision.
                mv_unit.mv->x = mv_unit.mv->x << 1;
                mv_unit.mv->y = mv_unit.mv->y << 1;

                context_ptr->tf_16x16_block_error[idx_32x32 * 4 + idx_16x16] = INT_MAX;
                signed short mv_x      = (_MVXT(context_ptr->p_best_mv16x16[mv_index])) << 1;
                signed short mv_y      = (_MVYT(context_ptr->p_best_mv16x16[mv_index])) << 1;
                signed short best_mv_x = mv_x;
                signed short best_mv_y = mv_y;

#if TUNE_REDESIGN_TF_CTRLS
                if (!pcs_ptr->tf_ctrls.half_pel_mode && !pcs_ptr->tf_ctrls.quarter_pel_mode && !pcs_ptr->tf_ctrls.eight_pel_mode)
                {
                    mv_unit.mv->x = mv_x;
                    mv_unit.mv->y = mv_y;

                    av1_inter_prediction(
#if FTR_SCALE_FACTOR
                        scs_ptr,
#endif
                        NULL, //pcs_ptr,
                        (uint32_t)interp_filters,
                        &blk_ptr,
                        0, //ref_frame_type,
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
                        pu_origin_x,
                        pu_origin_y,
                        bsize,
                        bsize,
                        !is_highbd ? pic_ptr_ref : &reference_ptr,
                        NULL, //ref_pic_list1,
                        &prediction_ptr,
                        local_origin_x,
                        local_origin_y,
                        0, //perform_chroma,
                        (uint8_t)encoder_bit_depth);

                    uint64_t distortion;
                    if (!is_highbd) {
                        uint8_t *pred_y_ptr = pred[C_Y] + bsize * idx_y * stride_pred[C_Y] +
                            bsize * idx_x;
                        uint8_t *src_y_ptr = src[C_Y] + bsize * idx_y * stride_src[C_Y] +
                            bsize * idx_x;

                        const AomVarianceFnPtr *fn_ptr = &mefn_ptr[BLOCK_16X16];

                        unsigned int sse;
                        distortion = fn_ptr->vf(
                            pred_y_ptr, stride_pred[C_Y], src_y_ptr, stride_src[C_Y], &sse);
                    }
                    else {
                        uint16_t *pred_y_ptr = pred_16bit[C_Y] +
                            bsize * idx_y * stride_pred[C_Y] + bsize * idx_x;
                        uint16_t *src_y_ptr = src_16bit[C_Y] + bsize * idx_y * stride_src[C_Y] +
                            bsize * idx_x;

                        unsigned int sse;
                        distortion = variance_highbd(pred_y_ptr,
                            stride_pred[C_Y],
                            src_y_ptr,
                            stride_src[C_Y],
                            16,
                            16,
                            &sse);
                    }
                    if (distortion <
                        context_ptr->tf_16x16_block_error[idx_32x32 * 4 + idx_16x16]) {
                        context_ptr->tf_16x16_block_error[idx_32x32 * 4 + idx_16x16] =
                            distortion;
                        best_mv_x = mv_unit.mv->x;
                        best_mv_y = mv_unit.mv->y;
                    }
                }
#endif
                // Perform 1/2 Pel MV Refinement
#if FTR_OPTIMISE_TF
#if !TUNE_REDESIGN_TF_CTRLS
                if (pcs_ptr->tf_ctrls.bypass_halfpel) {
#endif
#endif
                    for (signed short i = -4; i <= 4; i = i + 4) {
                        for (signed short j = -4; j <= 4; j = j + 4) {
#if TUNE_REDESIGN_TF_CTRLS
                            if (pcs_ptr->tf_ctrls.half_pel_mode == 2 && i != 0 && j != 0)
                                continue;
#endif
                            mv_unit.mv->x = mv_x + i;
                            mv_unit.mv->y = mv_y + j;

                            av1_inter_prediction(
#if FTR_SCALE_FACTOR
                                scs_ptr,
#endif
                                NULL, //pcs_ptr,
                                (uint32_t)interp_filters,
                                &blk_ptr,
                                0, //ref_frame_type,
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
                                pu_origin_x,
                                pu_origin_y,
                                bsize,
                                bsize,
                                !is_highbd ? pic_ptr_ref : &reference_ptr,
                                NULL, //ref_pic_list1,
                                &prediction_ptr,
                                local_origin_x,
                                local_origin_y,
                                0, //perform_chroma,
                                (uint8_t)encoder_bit_depth);

                            uint64_t distortion;
                            if (!is_highbd) {
                                uint8_t *pred_y_ptr = pred[C_Y] + bsize * idx_y * stride_pred[C_Y] +
                                    bsize * idx_x;
                                uint8_t *src_y_ptr = src[C_Y] + bsize * idx_y * stride_src[C_Y] +
                                    bsize * idx_x;

                                const AomVarianceFnPtr *fn_ptr = &mefn_ptr[BLOCK_16X16];

                                unsigned int sse;
                                distortion = fn_ptr->vf(
                                    pred_y_ptr, stride_pred[C_Y], src_y_ptr, stride_src[C_Y], &sse);
                            }
                            else {
                                uint16_t *pred_y_ptr = pred_16bit[C_Y] +
                                    bsize * idx_y * stride_pred[C_Y] + bsize * idx_x;
                                uint16_t *src_y_ptr = src_16bit[C_Y] + bsize * idx_y * stride_src[C_Y] +
                                    bsize * idx_x;

                                unsigned int sse;
                                distortion = variance_highbd(pred_y_ptr,
                                    stride_pred[C_Y],
                                    src_y_ptr,
                                    stride_src[C_Y],
                                    16,
                                    16,
                                    &sse);
                            }
                            if (distortion <
                                context_ptr->tf_16x16_block_error[idx_32x32 * 4 + idx_16x16]) {
                                context_ptr->tf_16x16_block_error[idx_32x32 * 4 + idx_16x16] =
                                    distortion;
                                best_mv_x = mv_unit.mv->x;
                                best_mv_y = mv_unit.mv->y;
                            }
                        }
                    }
#if FTR_OPTIMISE_TF
#if !TUNE_REDESIGN_TF_CTRLS
                }
#endif
#endif
                mv_x = best_mv_x;
                mv_y = best_mv_y;

                // Perform 1/4 Pel MV Refinement
                for (signed short i = -2; i <= 2; i = i + 2) {
                    for (signed short j = -2; j <= 2; j = j + 2) {

#if TUNE_REDESIGN_TF_CTRLS
                        if (pcs_ptr->tf_ctrls.quarter_pel_mode == 2 && i != 0 && j != 0)
                            continue;
#endif
                        mv_unit.mv->x = mv_x + i;
                        mv_unit.mv->y = mv_y + j;

                        av1_inter_prediction(
#if FTR_SCALE_FACTOR
                                            scs_ptr,
#endif
                                             NULL, //pcs_ptr,
                                             (uint32_t)interp_filters,
                                             &blk_ptr,
                                             0, //ref_frame_type,
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
                                             pu_origin_x,
                                             pu_origin_y,
                                             bsize,
                                             bsize,
                                             !is_highbd ? pic_ptr_ref : &reference_ptr,
                                             NULL, //ref_pic_list1,
                                             &prediction_ptr,
                                             local_origin_x,
                                             local_origin_y,
                                             0, //perform_chroma,
                                             (uint8_t)encoder_bit_depth);

                        uint64_t distortion;
                        if (!is_highbd) {
                            uint8_t *pred_y_ptr = pred[C_Y] + bsize * idx_y * stride_pred[C_Y] +
                                bsize * idx_x;
                            uint8_t *src_y_ptr = src[C_Y] + bsize * idx_y * stride_src[C_Y] +
                                bsize * idx_x;

                            const AomVarianceFnPtr *fn_ptr = &mefn_ptr[BLOCK_16X16];

                            unsigned int sse;
                            distortion = fn_ptr->vf(
                                pred_y_ptr, stride_pred[C_Y], src_y_ptr, stride_src[C_Y], &sse);
                        } else {
                            uint16_t *pred_y_ptr = pred_16bit[C_Y] +
                                bsize * idx_y * stride_pred[C_Y] + bsize * idx_x;
                            uint16_t *src_y_ptr = src_16bit[C_Y] + bsize * idx_y * stride_src[C_Y] +
                                bsize * idx_x;
                            ;

                            unsigned int sse;
                            distortion = variance_highbd(pred_y_ptr,
                                                         stride_pred[C_Y],
                                                         src_y_ptr,
                                                         stride_src[C_Y],
                                                         16,
                                                         16,
                                                         &sse);
                        }
                        if (distortion <
                            context_ptr->tf_16x16_block_error[idx_32x32 * 4 + idx_16x16]) {
                            context_ptr->tf_16x16_block_error[idx_32x32 * 4 + idx_16x16] =
                                distortion;
                            best_mv_x = mv_unit.mv->x;
                            best_mv_y = mv_unit.mv->y;
                        }
                    }
                }

                mv_x = best_mv_x;
                mv_y = best_mv_y;
                // Perform 1/8 Pel MV Refinement
#if TUNE_REDESIGN_TF_CTRLS
                if (pcs_ptr->tf_ctrls.eight_pel_mode)
#else
                if (context_ptr->tf_hp)
#endif
                    for (signed short i = -1; i <= 1; i++) {
                        for (signed short j = -1; j <= 1; j++) {

#if TUNE_REDESIGN_TF_CTRLS
                            if (pcs_ptr->tf_ctrls.eight_pel_mode == 2 && i != 0 && j != 0)
                                continue;
#endif
                            mv_unit.mv->x = mv_x + i;
                            mv_unit.mv->y = mv_y + j;

                            av1_inter_prediction(
#if FTR_SCALE_FACTOR
                                                scs_ptr,
#endif
                                                 NULL, //pcs_ptr,
                                                 (uint32_t)interp_filters,
                                                 &blk_ptr,
                                                 0, //ref_frame_type,
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
                                                 pu_origin_x,
                                                 pu_origin_y,
                                                 bsize,
                                                 bsize,
                                                 !is_highbd ? pic_ptr_ref : &reference_ptr,
                                                 NULL, //ref_pic_list1,
                                                 &prediction_ptr,
                                                 local_origin_x,
                                                 local_origin_y,
                                                 0, //perform_chroma,
                                                 (uint8_t)encoder_bit_depth);

                            uint64_t distortion;
                            if (!is_highbd) {
                                uint8_t *pred_y_ptr = pred[C_Y] + bsize * idx_y * stride_pred[C_Y] +
                                    bsize * idx_x;
                                uint8_t *src_y_ptr = src[C_Y] + bsize * idx_y * stride_src[C_Y] +
                                    bsize * idx_x;

                                const AomVarianceFnPtr *fn_ptr = &mefn_ptr[BLOCK_16X16];

                                unsigned int sse;
                                distortion = fn_ptr->vf(
                                    pred_y_ptr, stride_pred[C_Y], src_y_ptr, stride_src[C_Y], &sse);
                            } else {
                                uint16_t *pred_y_ptr = pred_16bit[C_Y] +
                                    bsize * idx_y * stride_pred[C_Y] + bsize * idx_x;
                                uint16_t *src_y_ptr = src_16bit[C_Y] +
                                    bsize * idx_y * stride_src[C_Y] + bsize * idx_x;

                                unsigned int sse;
                                distortion = variance_highbd(pred_y_ptr,
                                                             stride_pred[C_Y],
                                                             src_y_ptr,
                                                             stride_src[C_Y],
                                                             16,
                                                             16,
                                                             &sse);
                            }
                            if (distortion <
                                context_ptr->tf_16x16_block_error[idx_32x32 * 4 + idx_16x16]) {
                                context_ptr->tf_16x16_block_error[idx_32x32 * 4 + idx_16x16] =
                                    distortion;
                                best_mv_x = mv_unit.mv->x;
                                best_mv_y = mv_unit.mv->y;
                            }
                        }
                    }
                context_ptr->tf_16x16_mv_x[idx_32x32 * 4 + idx_16x16] = best_mv_x;
                context_ptr->tf_16x16_mv_y[idx_32x32 * 4 + idx_16x16] = best_mv_y;
            }
#if !TUNE_REDESIGN_TF_CTRLS
        }
#endif
}

static void tf_32x32_sub_pel_search(PictureParentControlSet *pcs_ptr, MeContext *context_ptr,
                                    PictureParentControlSet *pcs_ref,
                                    EbPictureBufferDesc *pic_ptr_ref, EbByte *pred,
                                    uint16_t **pred_16bit, uint32_t *stride_pred, EbByte *src,
                                    uint16_t **src_16bit, uint32_t *stride_src,
                                    uint32_t sb_origin_x, uint32_t sb_origin_y, uint32_t ss_x,
                                    int encoder_bit_depth) {

#if FTR_SCALE_FACTOR
    SequenceControlSet *scs_ptr =
        (SequenceControlSet *)pcs_ptr->scs_wrapper_ptr->object_ptr;
#endif

    InterpFilters interp_filters = av1_make_interp_filters(EIGHTTAP_REGULAR, EIGHTTAP_REGULAR);

    EbBool is_highbd = (encoder_bit_depth == 8) ? (uint8_t)EB_FALSE : (uint8_t)EB_TRUE;

    BlkStruct   blk_ptr;
    MacroBlockD av1xd;
    blk_ptr.av1xd = &av1xd;
    MvUnit mv_unit;
    mv_unit.pred_direction = UNI_PRED_LIST_0;

    EbPictureBufferDesc reference_ptr;
    EbPictureBufferDesc prediction_ptr;

    UNUSED(ss_x);

    prediction_ptr.origin_x  = 0;
    prediction_ptr.origin_y  = 0;
    prediction_ptr.stride_y  = BW;
    prediction_ptr.stride_cb = (uint16_t)BW >> ss_x;
    prediction_ptr.stride_cr = (uint16_t)BW >> ss_x;

    if (!is_highbd) {
        assert(src[C_Y] != NULL);
        if (context_ptr->tf_chroma) {
            assert(src[C_U] != NULL);
            assert(src[C_V] != NULL);
        }
        prediction_ptr.buffer_y  = pred[C_Y];
        prediction_ptr.buffer_cb = pred[C_U];
        prediction_ptr.buffer_cr = pred[C_V];
    } else {
        assert(src_16bit[C_Y] != NULL);
        if (context_ptr->tf_chroma) {
            assert(src_16bit[C_U] != NULL);
            assert(src_16bit[C_V] != NULL);
        }
        prediction_ptr.buffer_y  = (uint8_t *)pred_16bit[C_Y];
        prediction_ptr.buffer_cb = (uint8_t *)pred_16bit[C_U];
        prediction_ptr.buffer_cr = (uint8_t *)pred_16bit[C_V];
        reference_ptr.buffer_y   = (uint8_t *)pcs_ref->altref_buffer_highbd[C_Y];
        reference_ptr.buffer_cb  = (uint8_t *)pcs_ref->altref_buffer_highbd[C_U];
        reference_ptr.buffer_cr  = (uint8_t *)pcs_ref->altref_buffer_highbd[C_V];
        reference_ptr.origin_x   = pic_ptr_ref->origin_x;
        reference_ptr.origin_y   = pic_ptr_ref->origin_y;
        reference_ptr.stride_y   = pic_ptr_ref->stride_y;
        reference_ptr.stride_cb  = pic_ptr_ref->stride_cb;
        reference_ptr.stride_cr  = pic_ptr_ref->stride_cr;
        reference_ptr.width      = pic_ptr_ref->width;
        reference_ptr.height     = pic_ptr_ref->height;
    }

    uint32_t bsize = 32;
#if TUNE_REDESIGN_TF_CTRLS
    uint32_t idx_32x32 = context_ptr->idx_32x32;
#else
    for (uint32_t idx_32x32 = 0; idx_32x32 < 4; idx_32x32++) {
#endif
        uint32_t idx_x = idx_32x32 & 0x1;
        uint32_t idx_y = idx_32x32 >> 1;

        uint16_t local_origin_x = idx_x * bsize;
        uint16_t local_origin_y = idx_y * bsize;
        uint16_t pu_origin_x    = sb_origin_x + local_origin_x;
        uint16_t pu_origin_y    = sb_origin_y + local_origin_y;
        uint32_t mirow          = pu_origin_y >> MI_SIZE_LOG2;
        uint32_t micol          = pu_origin_x >> MI_SIZE_LOG2;
        blk_ptr.mds_idx         = get_mds_idx(local_origin_x,
                                      local_origin_y,
                                      bsize,
                                      pcs_ptr->scs_ptr->seq_header.sb_size == BLOCK_128X128);

        const int32_t bw                 = mi_size_wide[BLOCK_32X32];
        const int32_t bh                 = mi_size_high[BLOCK_32X32];
        blk_ptr.av1xd->mb_to_top_edge    = -(int32_t)((mirow * MI_SIZE) * 8);
        blk_ptr.av1xd->mb_to_bottom_edge = ((pcs_ptr->av1_cm->mi_rows - bw - mirow) * MI_SIZE) * 8;
        blk_ptr.av1xd->mb_to_left_edge   = -(int32_t)((micol * MI_SIZE) * 8);
        blk_ptr.av1xd->mb_to_right_edge  = ((pcs_ptr->av1_cm->mi_cols - bh - micol) * MI_SIZE) * 8;

        uint32_t mv_index = idx_32x32;
        mv_unit.mv->x     = _MVXT(context_ptr->p_best_mv32x32[mv_index]);
        mv_unit.mv->y     = _MVYT(context_ptr->p_best_mv32x32[mv_index]);
        // AV1 MVs are always in 1/8th pel precision.
        mv_unit.mv->x = mv_unit.mv->x << 1;
        mv_unit.mv->y = mv_unit.mv->y << 1;

        context_ptr->tf_32x32_block_error[idx_32x32] = INT_MAX;
        signed short mv_x      = (_MVXT(context_ptr->p_best_mv32x32[mv_index])) << 1;
        signed short mv_y      = (_MVYT(context_ptr->p_best_mv32x32[mv_index])) << 1;
        signed short best_mv_x = mv_x;
        signed short best_mv_y = mv_y;

#if TUNE_REDESIGN_TF_CTRLS
        if (!pcs_ptr->tf_ctrls.half_pel_mode && !pcs_ptr->tf_ctrls.quarter_pel_mode && !pcs_ptr->tf_ctrls.eight_pel_mode)
        {
            mv_unit.mv->x = mv_x;
            mv_unit.mv->y = mv_y;

            av1_inter_prediction(
#if FTR_SCALE_FACTOR
                scs_ptr,
#endif
                NULL, //pcs_ptr,
                (uint32_t)interp_filters,
                &blk_ptr,
                0, //ref_frame_type,
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
                pu_origin_x,
                pu_origin_y,
                bsize,
                bsize,
                !is_highbd ? pic_ptr_ref : &reference_ptr,
                NULL, //ref_pic_list1,
                &prediction_ptr,
                local_origin_x,
                local_origin_y,
                0, //perform_chroma,
                (uint8_t)encoder_bit_depth);

            uint64_t distortion;
            if (!is_highbd) {
                uint8_t *pred_y_ptr = pred[C_Y] + bsize * idx_y * stride_pred[C_Y] +
                    bsize * idx_x;
                uint8_t *src_y_ptr = src[C_Y] + bsize * idx_y * stride_src[C_Y] + bsize * idx_x;

                const AomVarianceFnPtr *fn_ptr = &mefn_ptr[BLOCK_32X32];

                unsigned int sse;
                distortion = fn_ptr->vf(
                    pred_y_ptr, stride_pred[C_Y], src_y_ptr, stride_src[C_Y], &sse);
            }
            else {
                uint16_t *pred_y_ptr = pred_16bit[C_Y] + bsize * idx_y * stride_pred[C_Y] +
                    bsize * idx_x;
                uint16_t *src_y_ptr = src_16bit[C_Y] + bsize * idx_y * stride_src[C_Y] +
                    bsize * idx_x;
                ;

                unsigned int sse;
                distortion = variance_highbd(
                    pred_y_ptr, stride_pred[C_Y], src_y_ptr, stride_src[C_Y], 32, 32, &sse);
            }
            if (distortion < context_ptr->tf_32x32_block_error[idx_32x32]) {
                context_ptr->tf_32x32_block_error[idx_32x32] = distortion;
                best_mv_x = mv_unit.mv->x;
                best_mv_y = mv_unit.mv->y;
            }
        }
#endif
        // Perform 1/2 Pel MV Refinement
#if FTR_OPTIMISE_TF
#if !TUNE_REDESIGN_TF_CTRLS
        if (pcs_ptr->tf_ctrls.bypass_halfpel) {
#endif
#endif
#if TUNE_REDESIGN_TF_CTRLS
            if (pcs_ptr->tf_ctrls.half_pel_mode)
#endif
            for (signed short i = -4; i <= 4; i = i + 4) {
                for (signed short j = -4; j <= 4; j = j + 4) {

#if TUNE_REDESIGN_TF_CTRLS
                    if (pcs_ptr->tf_ctrls.half_pel_mode == 2 && i != 0 && j != 0)
                        continue;
#endif
                    mv_unit.mv->x = mv_x + i;
                    mv_unit.mv->y = mv_y + j;

                    av1_inter_prediction(
#if FTR_SCALE_FACTOR
                        scs_ptr,
#endif
                        NULL, //pcs_ptr,
                        (uint32_t)interp_filters,
                        &blk_ptr,
                        0, //ref_frame_type,
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
                        pu_origin_x,
                        pu_origin_y,
                        bsize,
                        bsize,
                        !is_highbd ? pic_ptr_ref : &reference_ptr,
                        NULL, //ref_pic_list1,
                        &prediction_ptr,
                        local_origin_x,
                        local_origin_y,
                        0, //perform_chroma,
                        (uint8_t)encoder_bit_depth);

                    uint64_t distortion;
                    if (!is_highbd) {
                        uint8_t *pred_y_ptr = pred[C_Y] + bsize * idx_y * stride_pred[C_Y] +
                            bsize * idx_x;
                        uint8_t *src_y_ptr = src[C_Y] + bsize * idx_y * stride_src[C_Y] + bsize * idx_x;

                        const AomVarianceFnPtr *fn_ptr = &mefn_ptr[BLOCK_32X32];

                        unsigned int sse;
                        distortion = fn_ptr->vf(
                            pred_y_ptr, stride_pred[C_Y], src_y_ptr, stride_src[C_Y], &sse);
                    }
                    else {
                        uint16_t *pred_y_ptr = pred_16bit[C_Y] + bsize * idx_y * stride_pred[C_Y] +
                            bsize * idx_x;
                        uint16_t *src_y_ptr = src_16bit[C_Y] + bsize * idx_y * stride_src[C_Y] +
                            bsize * idx_x;
                        ;

                        unsigned int sse;
                        distortion = variance_highbd(
                            pred_y_ptr, stride_pred[C_Y], src_y_ptr, stride_src[C_Y], 32, 32, &sse);
                    }
                    if (distortion < context_ptr->tf_32x32_block_error[idx_32x32]) {
                        context_ptr->tf_32x32_block_error[idx_32x32] = distortion;
                        best_mv_x = mv_unit.mv->x;
                        best_mv_y = mv_unit.mv->y;
                    }
                }
            }
#if !TUNE_REDESIGN_TF_CTRLS
#if FTR_OPTIMISE_TF
        }
#endif
#endif
        mv_x = best_mv_x;
        mv_y = best_mv_y;

        // Perform 1/4 Pel MV Refinement
#if TUNE_REDESIGN_TF_CTRLS
        if (pcs_ptr->tf_ctrls.quarter_pel_mode)
#endif
        for (signed short i = -2; i <= 2; i = i + 2) {
            for (signed short j = -2; j <= 2; j = j + 2) {

#if TUNE_REDESIGN_TF_CTRLS
                if (pcs_ptr->tf_ctrls.quarter_pel_mode == 2 && i != 0 && j != 0)
                    continue;
#endif
                mv_unit.mv->x = mv_x + i;
                mv_unit.mv->y = mv_y + j;

                av1_inter_prediction(
#if FTR_SCALE_FACTOR
                                     scs_ptr,
#endif
                                     NULL, //pcs_ptr,
                                     (uint32_t)interp_filters,
                                     &blk_ptr,
                                     0, //ref_frame_type,
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
                                     pu_origin_x,
                                     pu_origin_y,
                                     bsize,
                                     bsize,
                                     !is_highbd ? pic_ptr_ref : &reference_ptr,
                                     NULL, //ref_pic_list1,
                                     &prediction_ptr,
                                     local_origin_x,
                                     local_origin_y,
                                     0, //perform_chroma,
                                     (uint8_t)encoder_bit_depth);

                uint64_t distortion;
                if (!is_highbd) {
                    uint8_t *pred_y_ptr = pred[C_Y] + bsize * idx_y * stride_pred[C_Y] +
                        bsize * idx_x;
                    uint8_t *src_y_ptr = src[C_Y] + bsize * idx_y * stride_src[C_Y] + bsize * idx_x;

                    const AomVarianceFnPtr *fn_ptr = &mefn_ptr[BLOCK_32X32];

                    unsigned int sse;
                    distortion = fn_ptr->vf(
                        pred_y_ptr, stride_pred[C_Y], src_y_ptr, stride_src[C_Y], &sse);
                } else {
                    uint16_t *pred_y_ptr = pred_16bit[C_Y] + bsize * idx_y * stride_pred[C_Y] +
                        bsize * idx_x;
                    uint16_t *src_y_ptr = src_16bit[C_Y] + bsize * idx_y * stride_src[C_Y] +
                        bsize * idx_x;

                    unsigned int sse;
                    distortion = variance_highbd(
                        pred_y_ptr, stride_pred[C_Y], src_y_ptr, stride_src[C_Y], 32, 32, &sse);
                }
                if (distortion < context_ptr->tf_32x32_block_error[idx_32x32]) {
                    context_ptr->tf_32x32_block_error[idx_32x32] = distortion;
                    best_mv_x                                    = mv_unit.mv->x;
                    best_mv_y                                    = mv_unit.mv->y;
                }
            }
        }

        mv_x = best_mv_x;
        mv_y = best_mv_y;
        // Perform 1/8 Pel MV Refinement
#if TUNE_REDESIGN_TF_CTRLS
        if (pcs_ptr->tf_ctrls.eight_pel_mode)
#else
        if (context_ptr->tf_hp)
#endif
            for (signed short i = -1; i <= 1; i++) {
                for (signed short j = -1; j <= 1; j++) {

#if TUNE_REDESIGN_TF_CTRLS
                    if (pcs_ptr->tf_ctrls.eight_pel_mode == 2 && i != 0 && j != 0)
                        continue;
#endif
                    mv_unit.mv->x = mv_x + i;
                    mv_unit.mv->y = mv_y + j;

                    av1_inter_prediction(
#if FTR_SCALE_FACTOR
                                         scs_ptr,
#endif
                                         NULL, //pcs_ptr,
                                         (uint32_t)interp_filters,
                                         &blk_ptr,
                                         0, //ref_frame_type,
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
                                         pu_origin_x,
                                         pu_origin_y,
                                         bsize,
                                         bsize,
                                         !is_highbd ? pic_ptr_ref : &reference_ptr,
                                         NULL, //ref_pic_list1,
                                         &prediction_ptr,
                                         local_origin_x,
                                         local_origin_y,
                                         0, //perform_chroma,
                                         (uint8_t)encoder_bit_depth);

                    uint64_t distortion;
                    if (!is_highbd) {
                        uint8_t *pred_y_ptr = pred[C_Y] + bsize * idx_y * stride_pred[C_Y] +
                            bsize * idx_x;
                        uint8_t *src_y_ptr = src[C_Y] + bsize * idx_y * stride_src[C_Y] +
                            bsize * idx_x;

                        const AomVarianceFnPtr *fn_ptr = &mefn_ptr[BLOCK_32X32];

                        unsigned int sse;
                        distortion = fn_ptr->vf(
                            pred_y_ptr, stride_pred[C_Y], src_y_ptr, stride_src[C_Y], &sse);
                    } else {
                        uint16_t *pred_y_ptr = pred_16bit[C_Y] + bsize * idx_y * stride_pred[C_Y] +
                            bsize * idx_x;
                        uint16_t *src_y_ptr = src_16bit[C_Y] + bsize * idx_y * stride_src[C_Y] +
                            bsize * idx_x;

                        unsigned int sse;
                        distortion = variance_highbd(
                            pred_y_ptr, stride_pred[C_Y], src_y_ptr, stride_src[C_Y], 32, 32, &sse);
                    }
                    if (distortion < context_ptr->tf_32x32_block_error[idx_32x32]) {
                        context_ptr->tf_32x32_block_error[idx_32x32] = distortion;
                        best_mv_x                                    = mv_unit.mv->x;
                        best_mv_y                                    = mv_unit.mv->y;
                    }
                }
            }
        context_ptr->tf_32x32_mv_x[idx_32x32] = best_mv_x;
        context_ptr->tf_32x32_mv_y[idx_32x32] = best_mv_y;
#if !TUNE_REDESIGN_TF_CTRLS
    }
#endif
}

#if TUNE_REDESIGN_TF_CTRLS
static void tf_32x32_inter_prediction(PictureParentControlSet *pcs_ptr, MeContext *context_ptr,
                                PictureParentControlSet *pcs_ref, EbPictureBufferDesc *pic_ptr_ref,
                                EbByte *pred, uint16_t **pred_16bit, uint32_t sb_origin_x,
                                uint32_t sb_origin_y, uint32_t ss_x, int encoder_bit_depth) {
#else
static void tf_inter_prediction(PictureParentControlSet *pcs_ptr, MeContext *context_ptr,
                                PictureParentControlSet *pcs_ref, EbPictureBufferDesc *pic_ptr_ref,
                                EbByte *pred, uint16_t **pred_16bit, uint32_t sb_origin_x,
                                uint32_t sb_origin_y, uint32_t ss_x, int encoder_bit_depth) {
#endif
#if FTR_SCALE_FACTOR
    SequenceControlSet *scs_ptr =
        (SequenceControlSet *)pcs_ptr->scs_wrapper_ptr->object_ptr;
#endif
    const InterpFilters interp_filters = av1_make_interp_filters(MULTITAP_SHARP, MULTITAP_SHARP);

    EbBool is_highbd = (encoder_bit_depth == 8) ? (uint8_t)EB_FALSE : (uint8_t)EB_TRUE;

    BlkStruct   blk_ptr;
    MacroBlockD av1xd;
    blk_ptr.av1xd = &av1xd;
    MvUnit mv_unit;
    mv_unit.pred_direction = UNI_PRED_LIST_0;

    EbPictureBufferDesc reference_ptr;
    EbPictureBufferDesc prediction_ptr;

#if !TUNE_REDESIGN_TF_CTRLS
    UNUSED(ss_x);
#endif

    prediction_ptr.origin_x  = 0;
    prediction_ptr.origin_y  = 0;
    prediction_ptr.stride_y  = BW;
    prediction_ptr.stride_cb = (uint16_t)BW >> ss_x;
    prediction_ptr.stride_cr = (uint16_t)BW >> ss_x;

    if (!is_highbd) {
        prediction_ptr.buffer_y  = pred[C_Y];
        prediction_ptr.buffer_cb = pred[C_U];
        prediction_ptr.buffer_cr = pred[C_V];
    } else {
        prediction_ptr.buffer_y  = (uint8_t *)pred_16bit[C_Y];
        prediction_ptr.buffer_cb = (uint8_t *)pred_16bit[C_U];
        prediction_ptr.buffer_cr = (uint8_t *)pred_16bit[C_V];
        reference_ptr.buffer_y   = (uint8_t *)pcs_ref->altref_buffer_highbd[C_Y];
        reference_ptr.buffer_cb  = (uint8_t *)pcs_ref->altref_buffer_highbd[C_U];
        reference_ptr.buffer_cr  = (uint8_t *)pcs_ref->altref_buffer_highbd[C_V];
        reference_ptr.origin_x   = pic_ptr_ref->origin_x;
        reference_ptr.origin_y   = pic_ptr_ref->origin_y;
        reference_ptr.stride_y   = pic_ptr_ref->stride_y;
        reference_ptr.stride_cb  = pic_ptr_ref->stride_cb;
        reference_ptr.stride_cr  = pic_ptr_ref->stride_cr;
        reference_ptr.width      = pic_ptr_ref->width;
        reference_ptr.height     = pic_ptr_ref->height;
    }

#if TUNE_REDESIGN_TF_CTRLS
    uint32_t idx_32x32 = context_ptr->idx_32x32;
    if (context_ptr->tf_32x32_block_split_flag[idx_32x32]) {
#else
    for (uint32_t idx_32x32 = 0; idx_32x32 < 4; idx_32x32++) {

        if (context_ptr->tf_32x32_block_split_flag[idx_32x32]) {
#endif
            uint32_t bsize = 16;

            for (uint32_t idx_16x16 = 0; idx_16x16 < 4; idx_16x16++) {
#if TUNE_REDESIGN_TF_CTRLS
                uint32_t pu_index = idx_32x32_to_idx_16x16[idx_32x32][idx_16x16];
#else
                uint32_t pu_index = index_16x16_from_subindexes[idx_32x32][idx_16x16];
#endif

                uint32_t idx_y          = subblock_xy_16x16[pu_index][0];
                uint32_t idx_x          = subblock_xy_16x16[pu_index][1];
                uint16_t local_origin_x = idx_x * bsize;
                uint16_t local_origin_y = idx_y * bsize;
                uint16_t pu_origin_x    = sb_origin_x + local_origin_x;
                uint16_t pu_origin_y    = sb_origin_y + local_origin_y;
                uint32_t mirow          = pu_origin_y >> MI_SIZE_LOG2;
                uint32_t micol          = pu_origin_x >> MI_SIZE_LOG2;
                blk_ptr.mds_idx         = get_mds_idx(
                    local_origin_x,
                    local_origin_y,
                    bsize,
                    pcs_ptr->scs_ptr->seq_header.sb_size == BLOCK_128X128);

                const int32_t bw                 = mi_size_wide[BLOCK_16X16];
                const int32_t bh                 = mi_size_high[BLOCK_16X16];
                blk_ptr.av1xd->mb_to_top_edge    = -(int32_t)((mirow * MI_SIZE) * 8);
                blk_ptr.av1xd->mb_to_bottom_edge = ((pcs_ptr->av1_cm->mi_rows - bw - mirow) *
                                                    MI_SIZE) *
                    8;
                blk_ptr.av1xd->mb_to_left_edge  = -(int32_t)((micol * MI_SIZE) * 8);
                blk_ptr.av1xd->mb_to_right_edge = ((pcs_ptr->av1_cm->mi_cols - bh - micol) *
                                                   MI_SIZE) *
                    8;
                // Perform final pass using the 1/8 MV
                //AV1 MVs are always in 1/8th pel precision.
                mv_unit.mv->x = context_ptr->tf_16x16_mv_x[idx_32x32 * 4 + idx_16x16];
                mv_unit.mv->y = context_ptr->tf_16x16_mv_y[idx_32x32 * 4 + idx_16x16];
                av1_inter_prediction(
#if FTR_SCALE_FACTOR
                                     scs_ptr,
#endif
                                     NULL, //pcs_ptr,
                                     (uint32_t)interp_filters,
                                     &blk_ptr,
                                     0, //ref_frame_type,
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
                                     pu_origin_x,
                                     pu_origin_y,
                                     bsize,
                                     bsize,
                                     !is_highbd ? pic_ptr_ref : &reference_ptr,
                                     NULL, //ref_pic_list1,
                                     &prediction_ptr,
                                     local_origin_x,
                                     local_origin_y,
                                     context_ptr->tf_chroma,
                                     (uint8_t)encoder_bit_depth);
            }
        } else {
            uint32_t bsize = 32;

            uint32_t idx_x = idx_32x32 & 0x1;
            uint32_t idx_y = idx_32x32 >> 1;

            uint16_t local_origin_x = idx_x * bsize;
            uint16_t local_origin_y = idx_y * bsize;
            uint16_t pu_origin_x    = sb_origin_x + local_origin_x;
            uint16_t pu_origin_y    = sb_origin_y + local_origin_y;
            uint32_t mirow          = pu_origin_y >> MI_SIZE_LOG2;
            uint32_t micol          = pu_origin_x >> MI_SIZE_LOG2;
            blk_ptr.mds_idx         = get_mds_idx(local_origin_x,
                                          local_origin_y,
                                          bsize,
                                          pcs_ptr->scs_ptr->seq_header.sb_size == BLOCK_128X128);

            const int32_t bw                 = mi_size_wide[BLOCK_32X32];
            const int32_t bh                 = mi_size_high[BLOCK_32X32];
            blk_ptr.av1xd->mb_to_top_edge    = -(int32_t)((mirow * MI_SIZE) * 8);
            blk_ptr.av1xd->mb_to_bottom_edge = ((pcs_ptr->av1_cm->mi_rows - bw - mirow) * MI_SIZE) *
                8;
            blk_ptr.av1xd->mb_to_left_edge  = -(int32_t)((micol * MI_SIZE) * 8);
            blk_ptr.av1xd->mb_to_right_edge = ((pcs_ptr->av1_cm->mi_cols - bh - micol) * MI_SIZE) *
                8;

            // Perform final pass using the 1/8 MV
            // AV1 MVs are always in 1/8th pel precision.
            mv_unit.mv->x = context_ptr->tf_32x32_mv_x[idx_32x32];
            mv_unit.mv->y = context_ptr->tf_32x32_mv_y[idx_32x32];

            av1_inter_prediction(
#if FTR_SCALE_FACTOR
                                 scs_ptr,
#endif
                                 NULL, //pcs_ptr,
                                 (uint32_t)interp_filters,
                                 &blk_ptr,
                                 0, //ref_frame_type,
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
                                 pu_origin_x,
                                 pu_origin_y,
                                 bsize,
                                 bsize,
                                 !is_highbd ? pic_ptr_ref : &reference_ptr,
                                 NULL, //ref_pic_list1,
                                 &prediction_ptr,
                                 local_origin_x,
                                 local_origin_y,
                                 context_ptr->tf_chroma,
                                 (uint8_t)encoder_bit_depth);
        }
#if !TUNE_REDESIGN_TF_CTRLS
    }
#endif
}
static void get_final_filtered_pixels(MeContext *context_ptr, EbByte *src_center_ptr_start,
                                      uint16_t **altref_buffer_highbd_start, uint32_t **accum,
                                      uint16_t **count, const uint32_t *stride,
                                      int blk_y_src_offset, int blk_ch_src_offset,
                                      uint16_t blk_width_ch, uint16_t blk_height_ch,
#if !TUNE_REDESIGN_TF_CTRLS
                                      uint64_t *filtered_sse, uint64_t *filtered_sse_uv,
#endif
                                      EbBool is_highbd) {
    int i, j, k;

    if (!is_highbd) {
        // Process luma
        int pos = blk_y_src_offset;
        for (i = 0, k = 0; i < BH; i++) {
            for (j = 0; j < BW; j++, k++) {
#if !TUNE_REDESIGN_TF_CTRLS
                (*filtered_sse) += (uint64_t)((int32_t)src_center_ptr_start[C_Y][pos] -
                                              (int32_t)OD_DIVU(accum[C_Y][k] + (count[C_Y][k] >> 1),
                                                               count[C_Y][k])) *
                    ((int32_t)src_center_ptr_start[C_Y][pos] -
                     (int32_t)OD_DIVU(accum[C_Y][k] + (count[C_Y][k] >> 1), count[C_Y][k]));
#endif
                src_center_ptr_start[C_Y][pos] = (uint8_t)OD_DIVU(
                    accum[C_Y][k] + (count[C_Y][k] >> 1), count[C_Y][k]);
                pos++;
            }
            pos += stride[C_Y] - BW;
        }
        // Process chroma
        if (context_ptr->tf_chroma) {
            pos = blk_ch_src_offset;
            for (i = 0, k = 0; i < blk_height_ch; i++) {
                for (j = 0; j < blk_width_ch; j++, k++) {
#if !TUNE_REDESIGN_TF_CTRLS
                    (*filtered_sse_uv) += (uint64_t)(
                                              (int32_t)src_center_ptr_start[C_U][pos] -
                                              (int32_t)OD_DIVU(accum[C_U][k] + (count[C_U][k] >> 1),
                                                               count[C_U][k])) *
                        ((int32_t)src_center_ptr_start[C_U][pos] -
                         (int32_t)OD_DIVU(accum[C_U][k] + (count[C_U][k] >> 1), count[C_U][k]));
                    (*filtered_sse_uv) += (uint64_t)(
                                              (int32_t)src_center_ptr_start[C_V][pos] -
                                              (int32_t)OD_DIVU(accum[C_V][k] + (count[C_V][k] >> 1),
                                                               count[C_V][k])) *
                        ((int32_t)src_center_ptr_start[C_V][pos] -
                         (int32_t)OD_DIVU(accum[C_V][k] + (count[C_V][k] >> 1), count[C_V][k]));
#endif
                    src_center_ptr_start[C_U][pos] = (uint8_t)OD_DIVU(
                        accum[C_U][k] + (count[C_U][k] >> 1), count[C_U][k]);
                    src_center_ptr_start[C_V][pos] = (uint8_t)OD_DIVU(
                        accum[C_V][k] + (count[C_V][k] >> 1), count[C_V][k]);
                    pos++;
                }
                pos += stride[C_U] - blk_width_ch;
            }
        }
    } else {
        // Process luma
        int pos = blk_y_src_offset;
        for (i = 0, k = 0; i < BH; i++) {
            for (j = 0; j < BW; j++, k++) {
#if !TUNE_REDESIGN_TF_CTRLS
                (*filtered_sse) += (uint64_t)((int32_t)altref_buffer_highbd_start[C_Y][pos] -
                                              (int32_t)OD_DIVU(accum[C_Y][k] + (count[C_Y][k] >> 1),
                                                               count[C_Y][k])) *
                    ((int32_t)altref_buffer_highbd_start[C_Y][pos] -
                     (int32_t)OD_DIVU(accum[C_Y][k] + (count[C_Y][k] >> 1), count[C_Y][k]));
#endif
                altref_buffer_highbd_start[C_Y][pos] = (uint16_t)OD_DIVU(
                    accum[C_Y][k] + (count[C_Y][k] >> 1), count[C_Y][k]);
                pos++;
            }
            pos += stride[C_Y] - BW;
        }
        // Process chroma
        if (context_ptr->tf_chroma) {
            pos = blk_ch_src_offset;
            for (i = 0, k = 0; i < blk_height_ch; i++) {
                for (j = 0; j < blk_width_ch; j++, k++) {
#if !TUNE_REDESIGN_TF_CTRLS
                    (*filtered_sse_uv) += (uint64_t)(
                                              (int32_t)altref_buffer_highbd_start[C_U][pos] -
                                              (int32_t)OD_DIVU(accum[C_U][k] + (count[C_U][k] >> 1),
                                                               count[C_U][k])) *
                        ((int32_t)altref_buffer_highbd_start[C_U][pos] -
                         (int32_t)OD_DIVU(accum[C_U][k] + (count[C_U][k] >> 1), count[C_U][k]));
                    (*filtered_sse_uv) += (uint64_t)(
                                              (int32_t)altref_buffer_highbd_start[C_V][pos] -
                                              (int32_t)OD_DIVU(accum[C_V][k] + (count[C_V][k] >> 1),
                                                               count[C_V][k])) *
                        ((int32_t)altref_buffer_highbd_start[C_V][pos] -
                         (int32_t)OD_DIVU(accum[C_V][k] + (count[C_V][k] >> 1), count[C_V][k]));
#endif
                    altref_buffer_highbd_start[C_U][pos] = (uint16_t)OD_DIVU(
                        accum[C_U][k] + (count[C_U][k] >> 1), count[C_U][k]);
                    altref_buffer_highbd_start[C_V][pos] = (uint16_t)OD_DIVU(
                        accum[C_V][k] + (count[C_V][k] >> 1), count[C_V][k]);
                    pos++;
                }
                pos += stride[C_U] - blk_width_ch;
            }
        }
    }
}

#if TUNE_REDESIGN_TF_CTRLS
/*
* Check whether to consider this reference frame(frame_index) @ the level of each 64x64 based on ME results
*/
int8_t skip_this_reference_frame(PictureParentControlSet *picture_control_set_ptr_central, PictureParentControlSet **list_picture_control_set_ptr, MeContext *context_ptr, int frame_index) {
    uint32_t  dist_16x16 = 0, dist_8x8 = 0;

    // 16x16
    for (unsigned i = 0; i < 16; i++) {
        dist_16x16 += context_ptr->p_best_sad_16x16[i];
    }

    // 8x8
    for (unsigned i = 0; i < 64; i++) {
        dist_8x8 += context_ptr->p_best_sad_8x8[i];
    }

    int64_t dev_16x16_to_8x8 =
        (int64_t)(((int64_t)MAX(dist_16x16, 1) - (int64_t)MAX(dist_8x8, 1)) * 100) /
        (int64_t)MAX(dist_8x8, 1);

    if (dev_16x16_to_8x8 > picture_control_set_ptr_central->tf_ctrls.me_16x16_to_8x8_dev_th)
    {
        int8_t delta = (int8_t)(list_picture_control_set_ptr[frame_index]->picture_number - picture_control_set_ptr_central->picture_number);
        if (delta < -(int8_t)picture_control_set_ptr_central->tf_ctrls.max_64x64_past_pics || delta >(int8_t) picture_control_set_ptr_central->tf_ctrls.max_64x64_future_pics)
        {
            return 1;
        }
    }
    return 0;
}
#endif
// Produce the filtered alt-ref picture
// - core function
static EbErrorType produce_temporally_filtered_pic(
    PictureParentControlSet **list_picture_control_set_ptr,
    EbPictureBufferDesc **list_input_picture_ptr, uint8_t index_center,
#if !TUNE_REDESIGN_TF_CTRLS
    uint64_t *filtered_sse,
    uint64_t *filtered_sse_uv,
 #endif
    MotionEstimationContext_t *me_context_ptr,
    const double *noise_levels, int32_t segment_index, EbBool is_highbd) {
    DECLARE_ALIGNED(16, uint32_t, accumulator[BLK_PELS * COLOR_CHANNELS]);
    DECLARE_ALIGNED(16, uint16_t, counter[BLK_PELS * COLOR_CHANNELS]);
    uint32_t *accum[COLOR_CHANNELS] = {
        accumulator, accumulator + BLK_PELS, accumulator + (BLK_PELS << 1)};
    uint16_t *count[COLOR_CHANNELS] = {counter, counter + BLK_PELS, counter + (BLK_PELS << 1)};

    EbByte    predictor       = {NULL};
    uint16_t *predictor_16bit = {NULL};
    if (!is_highbd)
        EB_MALLOC_ALIGNED_ARRAY(predictor, BLK_PELS * COLOR_CHANNELS);
    else
        EB_MALLOC_ALIGNED_ARRAY(predictor_16bit, BLK_PELS * COLOR_CHANNELS);
    EbByte    pred[COLOR_CHANNELS] = {predictor, predictor + BLK_PELS, predictor + (BLK_PELS << 1)};
    uint16_t *pred_16bit[COLOR_CHANNELS] = {
        predictor_16bit, predictor_16bit + BLK_PELS, predictor_16bit + (BLK_PELS << 1)};

    PictureParentControlSet *picture_control_set_ptr_central =
        list_picture_control_set_ptr[index_center];
    EbPictureBufferDesc *input_picture_ptr_central = list_input_picture_ptr[index_center];

    int encoder_bit_depth =
        (int)picture_control_set_ptr_central->scs_ptr->static_config.encoder_bit_depth;

    SequenceControlSet *scs_ptr = (SequenceControlSet *)picture_control_set_ptr_central->scs_ptr;

    // chroma subsampling
    uint32_t ss_x          = picture_control_set_ptr_central->scs_ptr->subsampling_x;
    uint32_t ss_y          = picture_control_set_ptr_central->scs_ptr->subsampling_y;
    uint16_t blk_width_ch  = (uint16_t)BW >> ss_x;
    uint16_t blk_height_ch = (uint16_t)BH >> ss_y;

    uint32_t blk_cols = (uint32_t)(input_picture_ptr_central->width + BW - 1) /
        BW; // I think only the part of the picture
    uint32_t blk_rows = (uint32_t)(input_picture_ptr_central->height + BH - 1) /
        BH; // that fits to the 32x32 blocks are actually filtered

    uint32_t stride[COLOR_CHANNELS]      = {input_picture_ptr_central->stride_y,
                                       input_picture_ptr_central->stride_cb,
                                       input_picture_ptr_central->stride_cr};
    uint32_t stride_pred[COLOR_CHANNELS] = {BW, blk_width_ch, blk_width_ch};

    MeContext *context_ptr = me_context_ptr->me_context_ptr;

    uint32_t x_seg_idx;
    uint32_t y_seg_idx;
    uint32_t picture_width_in_b64  = blk_cols;
    uint32_t picture_height_in_b64 = blk_rows;
    SEGMENT_CONVERT_IDX_TO_XY(segment_index,
                              x_seg_idx,
                              y_seg_idx,
                              picture_control_set_ptr_central->tf_segments_column_count);
    uint32_t x_b64_start_idx = SEGMENT_START_IDX(
        x_seg_idx, picture_width_in_b64, picture_control_set_ptr_central->tf_segments_column_count);
    uint32_t x_b64_end_idx = SEGMENT_END_IDX(
        x_seg_idx, picture_width_in_b64, picture_control_set_ptr_central->tf_segments_column_count);
    uint32_t y_b64_start_idx = SEGMENT_START_IDX(
        y_seg_idx, picture_height_in_b64, picture_control_set_ptr_central->tf_segments_row_count);
    uint32_t y_b64_end_idx = SEGMENT_END_IDX(
        y_seg_idx, picture_height_in_b64, picture_control_set_ptr_central->tf_segments_row_count);

    // first position of the frame buffer according to the index center
    EbByte src_center_ptr_start[COLOR_CHANNELS] = {
        input_picture_ptr_central->buffer_y +
            input_picture_ptr_central->origin_y * input_picture_ptr_central->stride_y +
            input_picture_ptr_central->origin_x,
        input_picture_ptr_central->buffer_cb +
            (input_picture_ptr_central->origin_y >> ss_y) * input_picture_ptr_central->stride_cb +
            (input_picture_ptr_central->origin_x >> ss_x),
        input_picture_ptr_central->buffer_cr +
            (input_picture_ptr_central->origin_y >> ss_y) * input_picture_ptr_central->stride_cr +
            (input_picture_ptr_central->origin_x >> ss_x),
    };

    uint16_t *altref_buffer_highbd_start[COLOR_CHANNELS] = {
        picture_control_set_ptr_central->altref_buffer_highbd[C_Y] +
            input_picture_ptr_central->origin_y * input_picture_ptr_central->stride_y +
            input_picture_ptr_central->origin_x,
        picture_control_set_ptr_central->altref_buffer_highbd[C_U] +
            (input_picture_ptr_central->origin_y >> ss_y) *
                input_picture_ptr_central->stride_bit_inc_cb +
            (input_picture_ptr_central->origin_x >> ss_x),
        picture_control_set_ptr_central->altref_buffer_highbd[C_V] +
            (input_picture_ptr_central->origin_y >> ss_y) *
                input_picture_ptr_central->stride_bit_inc_cr +
            (input_picture_ptr_central->origin_x >> ss_x),
    };

#if TUNE_REDESIGN_TF_CTRLS
    // Hyper-parameter for filter weight adjustment.
    int decay_control = (picture_control_set_ptr_central->scs_ptr->input_resolution <=
        INPUT_SIZE_480p_RANGE)
        ? 3
        : 4;
    // Decrease the filter strength for low QPs
    if (picture_control_set_ptr_central->scs_ptr->static_config.qp <= ALT_REF_QP_THRESH)
        decay_control--;
#endif
#if !TUNE_REDESIGN_TF_CTRLS
    *filtered_sse    = 0;
    *filtered_sse_uv = 0;
#endif

    for (uint32_t blk_row = y_b64_start_idx; blk_row < y_b64_end_idx; blk_row++) {
        for (uint32_t blk_col = x_b64_start_idx; blk_col < x_b64_end_idx; blk_col++) {
            int blk_y_src_offset  = (blk_col * BW) + (blk_row * BH) * stride[C_Y];
            int blk_ch_src_offset = (blk_col * blk_width_ch) +
                (blk_row * blk_height_ch) * stride[C_U];

            // reset accumulator and count
            memset(accumulator, 0, BLK_PELS * COLOR_CHANNELS * sizeof(accumulator[0]));
            memset(counter, 0, BLK_PELS * COLOR_CHANNELS * sizeof(counter[0]));

#if   SS_OPT_TF2_ME_COPY
            EbByte    src_center_ptr[COLOR_CHANNELS]           = {NULL};
            uint16_t *altref_buffer_highbd_ptr[COLOR_CHANNELS] = {NULL};
            if (!is_highbd) {
                src_center_ptr[C_Y] = src_center_ptr_start[C_Y] + blk_y_src_offset;
                if (context_ptr->tf_chroma) {
                    src_center_ptr[C_U] = src_center_ptr_start[C_U] + blk_ch_src_offset;
                    src_center_ptr[C_V] = src_center_ptr_start[C_V] + blk_ch_src_offset;
                }
            }
            else {
                altref_buffer_highbd_ptr[C_Y] = altref_buffer_highbd_start[C_Y] +
                    blk_y_src_offset;
                if (context_ptr->tf_chroma) {
                    altref_buffer_highbd_ptr[C_U] = altref_buffer_highbd_start[C_U] +
                        blk_ch_src_offset;
                    altref_buffer_highbd_ptr[C_V] = altref_buffer_highbd_start[C_V] +
                        blk_ch_src_offset;
                }
            }

            if (!is_highbd)
                apply_filtering_central(
                    context_ptr,
                    input_picture_ptr_central,
                    src_center_ptr,
                    accum,
                    count,
                    BW,
                    BH,
                    ss_x,
                    ss_y);
            else
                apply_filtering_central_highbd(
                    context_ptr,
                    input_picture_ptr_central,
                    altref_buffer_highbd_ptr,
                    accum,
                    count,
                    BW,
                    BH,
                    ss_x,
                    ss_y);
#endif

            // for every frame to filter
            for (int frame_index = 0;
                 frame_index < (picture_control_set_ptr_central->past_altref_nframes +
                                picture_control_set_ptr_central->future_altref_nframes + 1);
                 frame_index++) {

#if  ! SS_OPT_TF2_ME_COPY
                EbByte    src_center_ptr[COLOR_CHANNELS]           = {NULL};
                uint16_t *altref_buffer_highbd_ptr[COLOR_CHANNELS] = {NULL};
                if (!is_highbd) {
                    src_center_ptr[C_Y] = src_center_ptr_start[C_Y] + blk_y_src_offset;
                    src_center_ptr[C_U] = src_center_ptr_start[C_U] + blk_ch_src_offset;
                    src_center_ptr[C_V] = src_center_ptr_start[C_V] + blk_ch_src_offset;
                } else {
                    altref_buffer_highbd_ptr[C_Y] = altref_buffer_highbd_start[C_Y] +
                        blk_y_src_offset;
                    altref_buffer_highbd_ptr[C_U] = altref_buffer_highbd_start[C_U] +
                        blk_ch_src_offset;
                    altref_buffer_highbd_ptr[C_V] = altref_buffer_highbd_start[C_V] +
                        blk_ch_src_offset;
                }
#endif
                // ------------
                // Step 1: motion estimation + compensation
                // ------------
                me_context_ptr->me_context_ptr->tf_frame_index  = frame_index;
                me_context_ptr->me_context_ptr->tf_index_center = index_center;
                // if frame to process is the center frame
                if (frame_index == index_center) {
 #if !  SS_OPT_TF2_ME_COPY
#if TUNE_REDESIGN_TF_CTRLS
                    if (!is_highbd)
                        apply_filtering_central(
                            context_ptr,
                            input_picture_ptr_central,
                            src_center_ptr,
                            accum,
                            count,
                            BW,
                            BH,
                            ss_x,
                            ss_y);
                    else
                        apply_filtering_central_highbd(
                            context_ptr,
                            input_picture_ptr_central,
                            altref_buffer_highbd_ptr,
                            accum,
                            count,
                            BW,
                            BH,
                            ss_x,
                            ss_y);
#endif
#else
                    // skip MC (central frame)
                    if (!is_highbd) {
                        pic_copy_kernel_8bit(
                            src_center_ptr[C_Y], stride[C_Y], pred[C_Y], stride_pred[C_Y], BW, BH);
                        if (context_ptr->tf_chroma) {
                            pic_copy_kernel_8bit(src_center_ptr[C_U],
                                                 stride[C_U],
                                                 pred[C_U],
                                                 stride_pred[C_U],
                                                 blk_width_ch,
                                                 blk_height_ch);
                            pic_copy_kernel_8bit(src_center_ptr[C_V],
                                                 stride[C_V],
                                                 pred[C_V],
                                                 stride_pred[C_V],
                                                 blk_width_ch,
                                                 blk_height_ch);
                        }
                    } else {
                        pic_copy_kernel_16bit(altref_buffer_highbd_ptr[C_Y],
                                              stride[C_Y],
                                              pred_16bit[C_Y],
                                              stride_pred[C_Y],
                                              BW,
                                              BH);
                        if (context_ptr->tf_chroma) {
                            pic_copy_kernel_16bit(altref_buffer_highbd_ptr[C_U],
                                                  stride[C_U],
                                                  pred_16bit[C_U],
                                                  stride_pred[C_U],
                                                  blk_width_ch,
                                                  blk_height_ch);
                            pic_copy_kernel_16bit(altref_buffer_highbd_ptr[C_V],
                                                  stride[C_V],
                                                  pred_16bit[C_V],
                                                  stride_pred[C_V],
                                                  blk_width_ch,
                                                  blk_height_ch);
                        }
                    }
#endif

                } else {
                    // Initialize ME context
                    // When in_loop_me is on, we should not use any PA related stuff
                    if (scs_ptr->in_loop_me)
                        create_me_context_and_picture_control_inl(
                            me_context_ptr,
                            list_picture_control_set_ptr[frame_index],
                            list_picture_control_set_ptr[index_center],
                            input_picture_ptr_central,
                            blk_row,
                            blk_col,
                            ss_x,
                            ss_y);
                    else
                        create_me_context_and_picture_control(
                            me_context_ptr,
                            list_picture_control_set_ptr[frame_index],
                            list_picture_control_set_ptr[index_center],
                            input_picture_ptr_central,
                            blk_row,
                            blk_col,
                            ss_x,
                            ss_y);
                    context_ptr->num_of_list_to_search       = 0;
                    context_ptr->num_of_ref_pic_to_search[0] = 1;
                    context_ptr->num_of_ref_pic_to_search[1] = 0;
                    context_ptr->temporal_layer_index =
                        picture_control_set_ptr_central->temporal_layer_index;
                    context_ptr->is_used_as_reference_flag =
                        picture_control_set_ptr_central->is_used_as_reference_flag;

                    if (!scs_ptr->in_loop_me) {
                        EbPaReferenceObject *reference_object =
                            (EbPaReferenceObject *)context_ptr->alt_ref_reference_ptr;
                        context_ptr->me_ds_ref_array[0][0].picture_ptr =
                            reference_object->input_padded_picture_ptr;
#if OPT_ONE_BUFFER_DOWNSAMPLED
                        context_ptr->me_ds_ref_array[0][0].sixteenth_picture_ptr =
                            reference_object->sixteenth_downsampled_picture_ptr;
                        context_ptr->me_ds_ref_array[0][0].quarter_picture_ptr =
                            reference_object->quarter_downsampled_picture_ptr;
#else
                        context_ptr->me_ds_ref_array[0][0].sixteenth_picture_ptr =
                            (scs_ptr->down_sampling_method_me_search == ME_FILTERED_DOWNSAMPLED)
                            ? reference_object->sixteenth_filtered_picture_ptr
                            : reference_object->sixteenth_decimated_picture_ptr;
                        context_ptr->me_ds_ref_array[0][0].quarter_picture_ptr =
                            (scs_ptr->down_sampling_method_me_search == ME_FILTERED_DOWNSAMPLED)
                            ? reference_object->quarter_filtered_picture_ptr
                            : reference_object->quarter_decimated_picture_ptr;
#endif
                        context_ptr->me_ds_ref_array[0][0].picture_number =
                            reference_object->picture_number;
                    }

                    // Perform ME - context_ptr will store the outputs (MVs, buffers, etc)
                    // Block-based MC using open-loop HME + refinement
#if FTR_TPL_TR
                    MePcs *me_pcs = context_ptr->me_pcs;
                    fill_me_pcs_wraper(picture_control_set_ptr_central, me_pcs,0,0);
#endif
                    motion_estimate_sb(
#if FTR_TPL_TR
                        me_pcs,
#else
                        picture_control_set_ptr_central, // source picture control set -> references come from here
#endif
                        (uint32_t)blk_row * blk_cols + blk_col,
                        (uint32_t)blk_col * BW, // x block
                        (uint32_t)blk_row * BH, // y block
                        context_ptr,
                        input_picture_ptr_central); // source picture

#if TUNE_REDESIGN_TF_CTRLS
                    // Check whether to consider this reference frame (frame_index) @ the level of each 64x64 based on ME results
                    if (skip_this_reference_frame(picture_control_set_ptr_central, list_picture_control_set_ptr, context_ptr, frame_index))
                        continue;

                    // split filtering function into 32x32 blocks
                    // TODO: implement a 64x64 SIMD version
                    for (int block_row = 0; block_row < 2; block_row++) {
                        for (int block_col = 0; block_col < 2; block_col++) {

                            context_ptr->idx_32x32 = block_col + (block_row << 1);

                            // Perform TF sub-pel search for 32x32 blocks
                            tf_32x32_sub_pel_search(picture_control_set_ptr_central,
                                context_ptr,
                                list_picture_control_set_ptr[frame_index],
                                list_input_picture_ptr[frame_index],
                                pred,
                                pred_16bit,
                                stride_pred,
                                src_center_ptr,
                                altref_buffer_highbd_ptr,
                                stride,
                                (uint32_t)blk_col * BW,
                                (uint32_t)blk_row * BH,
                                ss_x,
                                encoder_bit_depth);

                            // Perform TF sub-pel search for 16x16 blocks
                            tf_16x16_sub_pel_search(picture_control_set_ptr_central,
                                context_ptr,
                                list_picture_control_set_ptr[frame_index],
                                list_input_picture_ptr[frame_index],
                                pred,
                                pred_16bit,
                                stride_pred,
                                src_center_ptr,
                                altref_buffer_highbd_ptr,
                                stride,
                                (uint32_t)blk_col * BW,
                                (uint32_t)blk_row * BH,
                                ss_x,
                                encoder_bit_depth);

                            // Derive tf_32x32_block_split_flag
                            if (context_ptr->tf_16x16_search_do[context_ptr->idx_32x32]) {
                                derive_tf_32x32_block_split_flag(context_ptr);
                            }
                            else {
                                context_ptr->tf_32x32_block_split_flag[context_ptr->idx_32x32] = 0;
                            }

                            // Perform MC using the information acquired using the ME step
                            tf_32x32_inter_prediction(picture_control_set_ptr_central,
                                context_ptr,
                                list_picture_control_set_ptr[frame_index],
                                list_input_picture_ptr[frame_index],
                                pred,
                                pred_16bit,
                                (uint32_t)blk_col * BW,
                                (uint32_t)blk_row * BH,
                                ss_x,
                                encoder_bit_depth);

                            context_ptr->tf_block_col = block_col;
                            context_ptr->tf_block_row = block_row;
                            apply_filtering_block_plane_wise(context_ptr,
                                block_row,
                                block_col,
                                src_center_ptr,
                                altref_buffer_highbd_ptr,
                                pred,
                                pred_16bit,
                                accum,
                                count,
                                stride,
                                stride_pred,
                                BW >> 1,
                                BH >> 1,
                                ss_x,
                                ss_y,
                                noise_levels,
                                decay_control,
                                encoder_bit_depth);
                        }
                    }
#else
                    // Perform TF sub-pel search for 32x32 blocks
                    tf_32x32_sub_pel_search(picture_control_set_ptr_central,
                                            context_ptr,
                                            list_picture_control_set_ptr[frame_index],
                                            list_input_picture_ptr[frame_index],
                                            pred,
                                            pred_16bit,
                                            stride_pred,
                                            src_center_ptr,
                                            altref_buffer_highbd_ptr,
                                            stride,
                                            (uint32_t)blk_col * BW,
                                            (uint32_t)blk_row * BH,
                                            ss_x,
                                            encoder_bit_depth);

                    // Perform TF sub-pel search for 16x16 blocks
                    tf_16x16_sub_pel_search(picture_control_set_ptr_central,
                                            context_ptr,
                                            list_picture_control_set_ptr[frame_index],
                                            list_input_picture_ptr[frame_index],
                                            pred,
                                            pred_16bit,
                                            stride_pred,
                                            src_center_ptr,
                                            altref_buffer_highbd_ptr,
                                            stride,
                                            (uint32_t)blk_col * BW,
                                            (uint32_t)blk_row * BH,
                                            ss_x,
                                            encoder_bit_depth);

                    // Derive tf_32x32_block_split_flag
                    derive_tf_32x32_block_split_flag(context_ptr);
                    // Perform MC using the information acquired using the ME step
                    tf_inter_prediction(picture_control_set_ptr_central,
                                        context_ptr,
                                        list_picture_control_set_ptr[frame_index],
                                        list_input_picture_ptr[frame_index],
                                        pred,
                                        pred_16bit,
                                        (uint32_t)blk_col * BW,
                                        (uint32_t)blk_row * BH,
                                        ss_x,
                                        encoder_bit_depth);
#endif
                }
#if !TUNE_REDESIGN_TF_CTRLS
                // ------------
                // Step 2: temporal filtering using the motion compensated blocks
                // ------------
                // Hyper-parameter for filter weight adjustment.
                int decay_control = (picture_control_set_ptr_central->scs_ptr->input_resolution <=
                                     INPUT_SIZE_480p_RANGE)
                    ? 3
                    : 4;
                // Decrease the filter strength for low QPs
                if (picture_control_set_ptr_central->scs_ptr->static_config.qp <= ALT_REF_QP_THRESH)
                    decay_control--;

                // if frame to process is the center frame
                if (frame_index == index_center) {
                    const int use_planewise_strategy = 1;
                    if (!is_highbd)
                        apply_filtering_central(context_ptr,
                                                pred,
                                                accum,
                                                count,
                                                BW,
                                                BH,
                                                ss_x,
                                                ss_y,
                                                use_planewise_strategy);
                    else
                        apply_filtering_central_highbd(context_ptr,
                                                       pred_16bit,
                                                       accum,
                                                       count,
                                                       BW,
                                                       BH,
                                                       ss_x,
                                                       ss_y,
                                                       use_planewise_strategy);
                } else {
                    // split filtering function into 32x32 blocks
                    // TODO: implement a 64x64 SIMD version
                    for (int block_row = 0; block_row < 2; block_row++) {
                        for (int block_col = 0; block_col < 2; block_col++) {
                            context_ptr->tf_block_col = block_col;
                            context_ptr->tf_block_row = block_row;
                            apply_filtering_block_plane_wise(context_ptr,
                                                             block_row,
                                                             block_col,
                                                             src_center_ptr,
                                                             altref_buffer_highbd_ptr,
                                                             pred,
                                                             pred_16bit,
                                                             accum,
                                                             count,
                                                             stride,
                                                             stride_pred,
                                                             BW >> 1,
                                                             BH >> 1,
                                                             ss_x,
                                                             ss_y,
                                                             noise_levels,
                                                             decay_control,
                                                             encoder_bit_depth);
                        }
                    }
                }
#endif
            }

            // Normalize filter output to produce temporally filtered frame
            get_final_filtered_pixels(context_ptr,
                                      src_center_ptr_start,
                                      altref_buffer_highbd_start,
                                      accum,
                                      count,
                                      stride,
                                      blk_y_src_offset,
                                      blk_ch_src_offset,
                                      blk_width_ch,
                                      blk_height_ch,
#if !TUNE_REDESIGN_TF_CTRLS
                                      filtered_sse,
                                      filtered_sse_uv,
#endif
                                      is_highbd);
        }
    }

    if (!is_highbd)
        EB_FREE_ALIGNED_ARRAY(predictor);
    else
        EB_FREE_ALIGNED_ARRAY(predictor_16bit);
    return EB_ErrorNone;
}

// This is an adaptation of the mehtod in the following paper:
// Shen-Chuan Tai, Shih-Ming Yang, "A fast method for image noise
// estimation using Laplacian operator and adaptive edge detection,"
// Proc. 3rd International Symposium on Communications, Control and
// Signal Processing, 2008, St Julians, Malta.
// Return noise estimate, or -1.0 if there was a failure
// function from libaom
// Standard bit depht input (=8 bits) to estimate the noise, I don't think there needs to be two methods for this
// Operates on the Y component only
double estimate_noise(const uint8_t *src, uint16_t width, uint16_t height, uint16_t stride_y) {
    int64_t sum = 0;
    int64_t num = 0;

    for (int i = 1; i < height - 1; ++i) {
        for (int j = 1; j < width - 1; ++j) {
            const int k = i * stride_y + j;
            // Sobel gradients
            const int g_x = (src[k - stride_y - 1] - src[k - stride_y + 1]) +
                (src[k + stride_y - 1] - src[k + stride_y + 1]) + 2 * (src[k - 1] - src[k + 1]);
            const int g_y = (src[k - stride_y - 1] - src[k + stride_y - 1]) +
                (src[k - stride_y + 1] - src[k + stride_y + 1]) +
                2 * (src[k - stride_y] - src[k + stride_y]);
            const int ga = abs(g_x) + abs(g_y);
            if (ga < EDGE_THRESHOLD) { // Do not consider edge pixels to estimate the noise
                // Find Laplacian
                const int v = 4 * src[k] -
                    2 * (src[k - 1] + src[k + 1] + src[k - stride_y] + src[k + stride_y]) +
                    (src[k - stride_y - 1] + src[k - stride_y + 1] + src[k + stride_y - 1] +
                     src[k + stride_y + 1]);
                sum += abs(v);
                ++num;
            }
        }
    }
    // If very few smooth pels, return -1 since the estimate is unreliable
    if (num < SMOOTH_THRESHOLD)
        return -1.0;

    const double sigma = (double)sum / (6 * num) * SQRT_PI_BY_2;

    return sigma;
}

// Noise estimation for highbd
double estimate_noise_highbd(const uint16_t *src, int width, int height, int stride, int bd) {
    int64_t sum = 0;
    int64_t num = 0;

    for (int i = 1; i < height - 1; ++i) {
        for (int j = 1; j < width - 1; ++j) {
            const int k = i * stride + j;
            // Sobel gradients
            const int g_x = (src[k - stride - 1] - src[k - stride + 1]) +
                (src[k + stride - 1] - src[k + stride + 1]) + 2 * (src[k - 1] - src[k + 1]);
            const int g_y = (src[k - stride - 1] - src[k + stride - 1]) +
                (src[k - stride + 1] - src[k + stride + 1]) +
                2 * (src[k - stride] - src[k + stride]);
            const int ga = ROUND_POWER_OF_TWO(abs(g_x) + abs(g_y),
                                              bd - 8); // divide by 2^2 and round up
            if (ga < EDGE_THRESHOLD) { // Do not consider edge pixels to estimate the noise
                // Find Laplacian
                const int v = 4 * src[k] -
                    2 * (src[k - 1] + src[k + 1] + src[k - stride] + src[k + stride]) +
                    (src[k - stride - 1] + src[k - stride + 1] + src[k + stride - 1] +
                     src[k + stride + 1]);
                sum += ROUND_POWER_OF_TWO(abs(v), bd - 8);
                ++num;
            }
        }
    }
    // If very few smooth pels, return -1 since the estimate is unreliable
    if (num < SMOOTH_THRESHOLD)
        return -1.0;

    const double sigma = (double)sum / (6 * num) * SQRT_PI_BY_2;
    return sigma;
}


#if !TUNE_REDESIGN_TF_CTRLS
// Adjust filtering parameters: strength and nframes
static void adjust_filter_strength(PictureParentControlSet *picture_control_set_ptr_central,

                                   double noise_level, uint8_t *altref_strength, EbBool is_highbd,
                                   uint32_t encoder_bit_depth) {
    int strength = *altref_strength, adj_strength = strength;

    // Adjust the strength of the temporal filtering
    // based on the amount of noise present in the frame
    // adjustment in the integer range [-2, 1]
    // if noiselevel < 0, it means that the estimation was
    // unsuccessful and therefore keep the strength as it was set
    if (noise_level > 0) {
        int noiselevel_adj;
        if (noise_level < 1.2)
            noiselevel_adj = -1;
        else if (noise_level < 4.0)
            noiselevel_adj = 0;
        else
            noiselevel_adj = 1;
        adj_strength += noiselevel_adj;
    }
    // Decrease the filter strength for low QPs
    if (picture_control_set_ptr_central->scs_ptr->static_config.qp <= ALT_REF_QP_THRESH) {
        adj_strength = adj_strength - 1;
    }
    if (adj_strength > 0)
        strength = adj_strength;
    else
        strength = 0;
    // if highbd, adjust filter strength strength = strength + 2*(bit depth - 8)
    if (is_highbd)
        strength = strength + 2 * (encoder_bit_depth - 8);

#if DEBUG_TF
    SVT_LOG("[DEBUG] noise level: %g, strength = %d, adj_strength = %d\n",
            noise_level,
            *altref_strength,
            strength);
#endif

    *altref_strength = (uint8_t)strength;

    // TODO: apply further refinements to the filter parameters according to 1st pass statistics
}
#endif
//  Inloop padding + decimation
static void pad_and_decimate_filtered_pic_inl(
    PictureParentControlSet *picture_control_set_ptr_central) {
    // reference structures (padded pictures + downsampled versions)
    SequenceControlSet *scs_ptr = (SequenceControlSet *)
                                      picture_control_set_ptr_central->scs_wrapper_ptr->object_ptr;
    EbPictureBufferDesc *input_picture_ptr = picture_control_set_ptr_central->enhanced_picture_ptr;

    pad_input_pictures(scs_ptr, input_picture_ptr);

    EbDownScaledObject *ds_obj = (EbDownScaledObject *)picture_control_set_ptr_central
                                     ->down_scaled_picture_wrapper_ptr->object_ptr;

    if (scs_ptr->down_sampling_method_me_search == ME_FILTERED_DOWNSAMPLED) {
        downsample_filtering_input_picture(picture_control_set_ptr_central,
                                           input_picture_ptr,
                                           (EbPictureBufferDesc *)ds_obj->quarter_picture_ptr,
                                           (EbPictureBufferDesc *)ds_obj->sixteenth_picture_ptr);
    } else {
        downsample_decimation_input_picture(picture_control_set_ptr_central,
                                            input_picture_ptr,
                                            (EbPictureBufferDesc *)ds_obj->quarter_picture_ptr,
                                            (EbPictureBufferDesc *)ds_obj->sixteenth_picture_ptr);
    }
}
void pad_and_decimate_filtered_pic(PictureParentControlSet *picture_control_set_ptr_central) {
    // reference structures (padded pictures + downsampled versions)
    SequenceControlSet *scs_ptr = (SequenceControlSet *)
                                      picture_control_set_ptr_central->scs_wrapper_ptr->object_ptr;
    EbPaReferenceObject *src_object = (EbPaReferenceObject *)picture_control_set_ptr_central
                                          ->pa_reference_picture_wrapper_ptr->object_ptr;
    EbPictureBufferDesc *padded_pic_ptr = src_object->input_padded_picture_ptr;
    {
        EbPictureBufferDesc *input_picture_ptr =
            picture_control_set_ptr_central->enhanced_picture_ptr;
        uint8_t *pa = padded_pic_ptr->buffer_y + padded_pic_ptr->origin_x +
            padded_pic_ptr->origin_y * padded_pic_ptr->stride_y;
        uint8_t *in = input_picture_ptr->buffer_y + input_picture_ptr->origin_x +
            input_picture_ptr->origin_y * input_picture_ptr->stride_y;
        // Refine the non-8 padding
        pad_picture_to_multiple_of_min_blk_size_dimensions(scs_ptr, input_picture_ptr);

        //Generate padding first, then copy
        generate_padding(&(input_picture_ptr->buffer_y[C_Y]),
                         input_picture_ptr->stride_y,
                         input_picture_ptr->width,
                         input_picture_ptr->height,
                         input_picture_ptr->origin_x,
                         input_picture_ptr->origin_y);
        // Padding chroma after altref
        generate_padding(input_picture_ptr->buffer_cb,
                         input_picture_ptr->stride_cb,
                         input_picture_ptr->width >> scs_ptr->subsampling_x,
                         input_picture_ptr->height >> scs_ptr->subsampling_y,
                         input_picture_ptr->origin_x >> scs_ptr->subsampling_x,
                         input_picture_ptr->origin_y >> scs_ptr->subsampling_y);
        generate_padding(input_picture_ptr->buffer_cr,
                         input_picture_ptr->stride_cr,
                         input_picture_ptr->width >> scs_ptr->subsampling_x,
                         input_picture_ptr->height >> scs_ptr->subsampling_y,
                         input_picture_ptr->origin_x >> scs_ptr->subsampling_x,
                         input_picture_ptr->origin_y >> scs_ptr->subsampling_y);
        for (uint32_t row = 0; row < input_picture_ptr->height; row++)
            svt_memcpy(pa + row * padded_pic_ptr->stride_y,
                       in + row * input_picture_ptr->stride_y,
                       sizeof(uint8_t) * input_picture_ptr->width);
    }
    generate_padding(&(padded_pic_ptr->buffer_y[C_Y]),
                     padded_pic_ptr->stride_y,
                     padded_pic_ptr->width,
                     padded_pic_ptr->height,
                     padded_pic_ptr->origin_x,
                     padded_pic_ptr->origin_y);

    // 1/4 & 1/16 input picture downsampling
#if OPT_ONE_BUFFER_DOWNSAMPLED
    if (scs_ptr->down_sampling_method_me_search == ME_FILTERED_DOWNSAMPLED) {
        downsample_filtering_input_picture(picture_control_set_ptr_central,
            padded_pic_ptr,
            src_object->quarter_downsampled_picture_ptr,
            src_object->sixteenth_downsampled_picture_ptr);
    }
    else {
        downsample_decimation_input_picture(picture_control_set_ptr_central,
            padded_pic_ptr,
            src_object->quarter_downsampled_picture_ptr,
            src_object->sixteenth_downsampled_picture_ptr);
    }
#else
    downsample_decimation_input_picture(picture_control_set_ptr_central,
                                        padded_pic_ptr,
                                        src_object->quarter_decimated_picture_ptr,
                                        src_object->sixteenth_decimated_picture_ptr);

    // 1/4 & 1/16 input picture downsampling through filtering
    if (scs_ptr->down_sampling_method_me_search == ME_FILTERED_DOWNSAMPLED)
        downsample_filtering_input_picture(picture_control_set_ptr_central,
                                           padded_pic_ptr,
                                           src_object->quarter_filtered_picture_ptr,
                                           src_object->sixteenth_filtered_picture_ptr);
#endif
}

// save original enchanced_picture_ptr buffer in a separate buffer (to be replaced by the temporally filtered pic)
static EbErrorType save_src_pic_buffers(PictureParentControlSet *picture_control_set_ptr_central,
                                        uint32_t ss_y, EbBool is_highbd) {
    // allocate memory for the copy of the original enhanced buffer
    EB_MALLOC_ARRAY(picture_control_set_ptr_central->save_enhanced_picture_ptr[C_Y],
                    picture_control_set_ptr_central->enhanced_picture_ptr->luma_size);
    EB_MALLOC_ARRAY(picture_control_set_ptr_central->save_enhanced_picture_ptr[C_U],
                    picture_control_set_ptr_central->enhanced_picture_ptr->chroma_size);
    EB_MALLOC_ARRAY(picture_control_set_ptr_central->save_enhanced_picture_ptr[C_V],
                    picture_control_set_ptr_central->enhanced_picture_ptr->chroma_size);

    // if highbd, allocate memory for the copy of the original enhanced buffer - bit inc
    if (is_highbd) {
        EB_MALLOC_ARRAY(picture_control_set_ptr_central->save_enhanced_picture_bit_inc_ptr[C_Y],
                        picture_control_set_ptr_central->enhanced_picture_ptr->luma_size);
        EB_MALLOC_ARRAY(picture_control_set_ptr_central->save_enhanced_picture_bit_inc_ptr[C_U],
                        picture_control_set_ptr_central->enhanced_picture_ptr->chroma_size);
        EB_MALLOC_ARRAY(picture_control_set_ptr_central->save_enhanced_picture_bit_inc_ptr[C_V],
                        picture_control_set_ptr_central->enhanced_picture_ptr->chroma_size);
    }

    // copy buffers
    // Y
    uint32_t height_y = (uint32_t)(
        picture_control_set_ptr_central->enhanced_picture_ptr->height +
        picture_control_set_ptr_central->enhanced_picture_ptr->origin_y +
        picture_control_set_ptr_central->enhanced_picture_ptr->origin_bot_y);
    uint32_t height_uv = (uint32_t)(
        (picture_control_set_ptr_central->enhanced_picture_ptr->height +
         picture_control_set_ptr_central->enhanced_picture_ptr->origin_y +
         picture_control_set_ptr_central->enhanced_picture_ptr->origin_bot_y) >>
        ss_y);

    assert(height_y * picture_control_set_ptr_central->enhanced_picture_ptr->stride_y ==
           picture_control_set_ptr_central->enhanced_picture_ptr->luma_size);
    assert(height_uv * picture_control_set_ptr_central->enhanced_picture_ptr->stride_cb ==
           picture_control_set_ptr_central->enhanced_picture_ptr->chroma_size);
    assert(height_uv * picture_control_set_ptr_central->enhanced_picture_ptr->stride_cr ==
           picture_control_set_ptr_central->enhanced_picture_ptr->chroma_size);

    pic_copy_kernel_8bit(picture_control_set_ptr_central->enhanced_picture_ptr->buffer_y,
                         picture_control_set_ptr_central->enhanced_picture_ptr->stride_y,
                         picture_control_set_ptr_central->save_enhanced_picture_ptr[C_Y],
                         picture_control_set_ptr_central->enhanced_picture_ptr->stride_y,
                         picture_control_set_ptr_central->enhanced_picture_ptr->stride_y,
                         height_y);

    pic_copy_kernel_8bit(picture_control_set_ptr_central->enhanced_picture_ptr->buffer_cb,
                         picture_control_set_ptr_central->enhanced_picture_ptr->stride_cb,
                         picture_control_set_ptr_central->save_enhanced_picture_ptr[C_U],
                         picture_control_set_ptr_central->enhanced_picture_ptr->stride_cb,
                         picture_control_set_ptr_central->enhanced_picture_ptr->stride_cb,
                         height_uv);

    pic_copy_kernel_8bit(picture_control_set_ptr_central->enhanced_picture_ptr->buffer_cr,
                         picture_control_set_ptr_central->enhanced_picture_ptr->stride_cr,
                         picture_control_set_ptr_central->save_enhanced_picture_ptr[C_V],
                         picture_control_set_ptr_central->enhanced_picture_ptr->stride_cr,
                         picture_control_set_ptr_central->enhanced_picture_ptr->stride_cr,
                         height_uv);

    if (is_highbd) {
        // if highbd, copy bit inc buffers
        // Y
        pic_copy_kernel_8bit(
            picture_control_set_ptr_central->enhanced_picture_ptr->buffer_bit_inc_y,
            picture_control_set_ptr_central->enhanced_picture_ptr->stride_bit_inc_y,
            picture_control_set_ptr_central->save_enhanced_picture_bit_inc_ptr[C_Y],
            picture_control_set_ptr_central->enhanced_picture_ptr->stride_bit_inc_y,
            picture_control_set_ptr_central->enhanced_picture_ptr->stride_bit_inc_y,
            height_y);
        // U
        pic_copy_kernel_8bit(
            picture_control_set_ptr_central->enhanced_picture_ptr->buffer_bit_inc_cb,
            picture_control_set_ptr_central->enhanced_picture_ptr->stride_bit_inc_cb,
            picture_control_set_ptr_central->save_enhanced_picture_bit_inc_ptr[C_U],
            picture_control_set_ptr_central->enhanced_picture_ptr->stride_bit_inc_cb,
            picture_control_set_ptr_central->enhanced_picture_ptr->stride_bit_inc_cb,
            height_uv);
        // V
        pic_copy_kernel_8bit(
            picture_control_set_ptr_central->enhanced_picture_ptr->buffer_bit_inc_cr,
            picture_control_set_ptr_central->enhanced_picture_ptr->stride_bit_inc_cr,
            picture_control_set_ptr_central->save_enhanced_picture_bit_inc_ptr[C_V],
            picture_control_set_ptr_central->enhanced_picture_ptr->stride_bit_inc_cr,
            picture_control_set_ptr_central->enhanced_picture_ptr->stride_bit_inc_cr,
            height_uv);
    }

    return EB_ErrorNone;
}

EbErrorType svt_av1_init_temporal_filtering(
    PictureParentControlSet ** list_picture_control_set_ptr,
    PictureParentControlSet *  picture_control_set_ptr_central,
    MotionEstimationContext_t *me_context_ptr, int32_t segment_index) {
#if TUNE_REDESIGN_TF_CTRLS
    uint8_t index_center;
#else
    uint8_t *            altref_strength_ptr, index_center;
#endif
    EbPictureBufferDesc *central_picture_ptr;

#if TUNE_REDESIGN_TF_CTRLS
    me_context_ptr->me_context_ptr->tf_chroma = picture_control_set_ptr_central->tf_ctrls.do_chroma;
#else
    me_context_ptr->me_context_ptr->tf_hp     = picture_control_set_ptr_central->tf_ctrls.hp;
    me_context_ptr->me_context_ptr->tf_chroma = picture_control_set_ptr_central->tf_ctrls.chroma;
    me_context_ptr->me_context_ptr->tf_block_32x32_16x16_th =
        picture_control_set_ptr_central->tf_ctrls.block_32x32_16x16_th;
#endif
#if !TUNE_REDESIGN_TF_CTRLS
    altref_strength_ptr = &(picture_control_set_ptr_central->altref_strength);
#endif

    // index of the central source frame
    index_center = picture_control_set_ptr_central->past_altref_nframes;

    // if this assertion does not fail (as I think it should not, then remove picture_control_set_ptr_central from the input parameters of init_temporal_filtering())
    assert(list_picture_control_set_ptr[index_center] == picture_control_set_ptr_central);

    // source central frame picture buffer
    central_picture_ptr = picture_control_set_ptr_central->enhanced_picture_ptr;

    uint32_t encoder_bit_depth =
        picture_control_set_ptr_central->scs_ptr->static_config.encoder_bit_depth;
    EbBool is_highbd = (encoder_bit_depth == 8) ? (uint8_t)EB_FALSE : (uint8_t)EB_TRUE;

    // chroma subsampling
    uint32_t ss_x         = picture_control_set_ptr_central->scs_ptr->subsampling_x;
    uint32_t ss_y         = picture_control_set_ptr_central->scs_ptr->subsampling_y;
    double * noise_levels = &(picture_control_set_ptr_central->noise_levels[0]);
    //only one performs any picture based prep
    svt_block_on_mutex(picture_control_set_ptr_central->temp_filt_mutex);
    if (picture_control_set_ptr_central->temp_filt_prep_done == 0) {
        picture_control_set_ptr_central->temp_filt_prep_done = 1;
#if !TUNE_REDESIGN_TF_CTRLS
        // adjust filter parameter based on the estimated noise of the picture
        adjust_filter_strength(picture_control_set_ptr_central,
                               noise_levels[0],
                               altref_strength_ptr,
                               is_highbd,
                               encoder_bit_depth);
#endif
        // Pad chroma reference samples - once only per picture
        for (int i = 0; i < (picture_control_set_ptr_central->past_altref_nframes +
                             picture_control_set_ptr_central->future_altref_nframes + 1);
             i++) {
            EbPictureBufferDesc *pic_ptr_ref =
                list_picture_control_set_ptr[i]->enhanced_picture_ptr;
            generate_padding_pic(pic_ptr_ref, ss_x, ss_y, is_highbd);
            //10bit: for all the reference pictures do the packing once at the beggining.
            if (is_highbd && i != picture_control_set_ptr_central->past_altref_nframes) {
                EB_MALLOC_ARRAY(list_picture_control_set_ptr[i]->altref_buffer_highbd[C_Y],
                                central_picture_ptr->luma_size);
                EB_MALLOC_ARRAY(list_picture_control_set_ptr[i]->altref_buffer_highbd[C_U],
                                central_picture_ptr->chroma_size);
                EB_MALLOC_ARRAY(list_picture_control_set_ptr[i]->altref_buffer_highbd[C_V],
                                central_picture_ptr->chroma_size);
                // pack byte buffers to 16 bit buffer
                pack_highbd_pic(pic_ptr_ref,
                                list_picture_control_set_ptr[i]->altref_buffer_highbd,
                                ss_x,
                                ss_y,
                                EB_TRUE);
            }
        }

        picture_control_set_ptr_central->temporal_filtering_on =
            EB_TRUE; // set temporal filtering flag ON for current picture

        // save original source picture (to be replaced by the temporally filtered pic)
        // if stat_report is enabled for PSNR computation
        if (picture_control_set_ptr_central->scs_ptr->static_config.stat_report) {
            save_src_pic_buffers(picture_control_set_ptr_central, ss_y, is_highbd);
        }
    }
    svt_release_mutex(picture_control_set_ptr_central->temp_filt_mutex);
    me_context_ptr->me_context_ptr->min_frame_size = MIN(
        picture_control_set_ptr_central->aligned_height,
        picture_control_set_ptr_central->aligned_width);
    // index of the central source frame
    // index_center = picture_control_set_ptr_central->past_altref_nframes;
    // populate source frames picture buffer list
    EbPictureBufferDesc *list_input_picture_ptr[ALTREF_MAX_NFRAMES] = {NULL};
    for (int i = 0; i < (picture_control_set_ptr_central->past_altref_nframes +
                         picture_control_set_ptr_central->future_altref_nframes + 1);
         i++)
        list_input_picture_ptr[i] = list_picture_control_set_ptr[i]->enhanced_picture_ptr;

#if !TUNE_REDESIGN_TF_CTRLS
    uint64_t filtered_sse, filtered_sse_uv;
#endif

    produce_temporally_filtered_pic(list_picture_control_set_ptr,
                                    list_input_picture_ptr,
                                    index_center,
#if !TUNE_REDESIGN_TF_CTRLS
                                    &filtered_sse,
                                    &filtered_sse_uv,
#endif
                                    me_context_ptr,
                                    noise_levels,
                                    segment_index,
                                    is_highbd);

    svt_block_on_mutex(picture_control_set_ptr_central->temp_filt_mutex);
    picture_control_set_ptr_central->temp_filt_seg_acc++;
#if !TUNE_REDESIGN_TF_CTRLS
    if (!is_highbd) {
        picture_control_set_ptr_central->filtered_sse += filtered_sse;
        picture_control_set_ptr_central->filtered_sse_uv += filtered_sse_uv;
    } else {
        picture_control_set_ptr_central->filtered_sse += filtered_sse >> 4;
        picture_control_set_ptr_central->filtered_sse_uv += filtered_sse_uv >> 4;
    }
#endif

    if (picture_control_set_ptr_central->temp_filt_seg_acc ==
        picture_control_set_ptr_central->tf_segments_total_count) {
#if DEBUG_TF
        if (!is_highbd)
            save_YUV_to_file("filtered_picture.yuv",
                             central_picture_ptr->buffer_y,
                             central_picture_ptr->buffer_cb,
                             central_picture_ptr->buffer_cr,
                             central_picture_ptr->width,
                             central_picture_ptr->height,
                             central_picture_ptr->stride_y,
                             central_picture_ptr->stride_cb,
                             central_picture_ptr->stride_cr,
                             central_picture_ptr->origin_y,
                             central_picture_ptr->origin_x,
                             ss_x,
                             ss_y);
        else
            save_YUV_to_file_highbd("filtered_picture.yuv",
                                    picture_control_set_ptr_central->altref_buffer_highbd[C_Y],
                                    picture_control_set_ptr_central->altref_buffer_highbd[C_U],
                                    picture_control_set_ptr_central->altref_buffer_highbd[C_V],
                                    central_picture_ptr->width,
                                    central_picture_ptr->height,
                                    central_picture_ptr->stride_y,
                                    central_picture_ptr->stride_cb,
                                    central_picture_ptr->stride_cb,
                                    central_picture_ptr->origin_y,
                                    central_picture_ptr->origin_x,
                                    ss_x,
                                    ss_y);
#endif

        if (is_highbd) {
            unpack_highbd_pic(picture_control_set_ptr_central->altref_buffer_highbd,
                              central_picture_ptr,
                              ss_x,
                              ss_y,
                              EB_TRUE);

            EB_FREE_ARRAY(picture_control_set_ptr_central->altref_buffer_highbd[C_Y]);
            EB_FREE_ARRAY(picture_control_set_ptr_central->altref_buffer_highbd[C_U]);
            EB_FREE_ARRAY(picture_control_set_ptr_central->altref_buffer_highbd[C_V]);
            for (int i = 0; i < (picture_control_set_ptr_central->past_altref_nframes +
                                 picture_control_set_ptr_central->future_altref_nframes + 1);
                 i++) {
                if (i != picture_control_set_ptr_central->past_altref_nframes) {
                    EB_FREE_ARRAY(list_picture_control_set_ptr[i]->altref_buffer_highbd[C_Y]);
                    EB_FREE_ARRAY(list_picture_control_set_ptr[i]->altref_buffer_highbd[C_U]);
                    EB_FREE_ARRAY(list_picture_control_set_ptr[i]->altref_buffer_highbd[C_V]);
                }
            }
        }

        // padding + decimation: even if highbd src, this is only performed on the 8 bit buffer (excluding the LSBs)
        if (picture_control_set_ptr_central->scs_ptr->in_loop_me)
            pad_and_decimate_filtered_pic_inl(picture_control_set_ptr_central);
        else
            pad_and_decimate_filtered_pic(picture_control_set_ptr_central);
#if !TUNE_REDESIGN_TF_CTRLS
        // Normalize the filtered SSE. Add 8 bit precision.
        picture_control_set_ptr_central->filtered_sse =
            (picture_control_set_ptr_central->filtered_sse << 8) / central_picture_ptr->width /
            central_picture_ptr->height;
        picture_control_set_ptr_central->filtered_sse_uv =
            ((picture_control_set_ptr_central->filtered_sse_uv << 8) /
             (central_picture_ptr->width >> ss_x) / (central_picture_ptr->height >> ss_y)) /
            2;
#endif

        // signal that temp filt is done
        svt_post_semaphore(picture_control_set_ptr_central->temp_filt_done_semaphore);
    }

    svt_release_mutex(picture_control_set_ptr_central->temp_filt_mutex);

    return EB_ErrorNone;
}
