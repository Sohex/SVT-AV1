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

#include <stdlib.h>
#include <string.h>

#include "EbMotionEstimationContext.h"
#include "EbUtility.h"

#if !OPT_BYPASS_ME_CAND
void motion_estimation_pred_unit_ctor(MePredUnit *pu) {
    pu->distortion = 0xFFFFFFFFull;

    pu->prediction_direction = UNI_PRED_LIST_0;
    return;
}
#endif
static void me_context_dctor(EbPtr p) {
    MeContext *obj = (MeContext *)p;
#if !SS_OPT_TF2_ME_COPY
    EB_FREE_ALIGNED_ARRAY(obj->quarter_sb_buffer);
#endif

    EB_FREE_ARRAY(obj->mvd_bits_array);
#if !OPT_BYPASS_ME_CAND
    EB_FREE_ARRAY(obj->me_candidate);
#endif
    EB_FREE_ARRAY(obj->p_eight_pos_sad16x16);
#if !SS_OPT_TF2_ME_COPY
    EB_FREE_ALIGNED_ARRAY(obj->sixteenth_sb_buffer);
#endif
#if  !OPT_ME_RES_SAD_LOOP
    EB_FREE_ALIGNED_ARRAY(obj->sb_buffer);
#endif
#if FTR_TPL_TR
    EB_FREE(obj->me_pcs);
#endif
}
EbErrorType me_context_ctor(MeContext *object_ptr) {
#if !OPT_BYPASS_ME_CAND
    uint32_t pu_index;
    uint32_t me_candidate_index;
#endif
    object_ptr->dctor = me_context_dctor;
#if FTR_TPL_TR
    EB_MALLOC(object_ptr->me_pcs, sizeof(MePcs));
#endif
#if  !OPT_ME_RES_SAD_LOOP
    // Intermediate SB-sized buffer to retain the input samples
    object_ptr->sb_buffer_stride = BLOCK_SIZE_64;
    EB_MALLOC_ALIGNED_ARRAY(object_ptr->sb_buffer, BLOCK_SIZE_64 * object_ptr->sb_buffer_stride);
#endif



#if !SS_OPT_TF2_ME_COPY
    object_ptr->quarter_sb_buffer_stride = (BLOCK_SIZE_64 >> 1);
    EB_MALLOC_ALIGNED_ARRAY(object_ptr->quarter_sb_buffer,
                            (BLOCK_SIZE_64 >> 1) * object_ptr->quarter_sb_buffer_stride);

    object_ptr->sixteenth_sb_buffer_stride = (BLOCK_SIZE_64 >> 2);
    EB_MALLOC_ALIGNED_ARRAY(object_ptr->sixteenth_sb_buffer,
                            (BLOCK_SIZE_64 >> 2) * object_ptr->sixteenth_sb_buffer_stride);
#endif
#if !OPT_ME_RES_SAD_LOOP
    EB_MEMSET(
        object_ptr->sb_buffer, 0, sizeof(uint8_t) * BLOCK_SIZE_64 * object_ptr->sb_buffer_stride);
#endif
#if !OPT_BYPASS_ME_CAND
    EB_MALLOC_ARRAY(object_ptr->me_candidate, MAX_PA_ME_CAND);
    for (pu_index = 0; pu_index < SQUARE_PU_COUNT; pu_index++) {
        for (me_candidate_index = 0; me_candidate_index < MAX_PA_ME_CAND; me_candidate_index++) {
            motion_estimation_pred_unit_ctor(
                &(object_ptr->me_candidate[me_candidate_index]).pu[pu_index]);
        }
    }
#endif
    EB_MALLOC_ARRAY(object_ptr->p_eight_pos_sad16x16,
                    8 * 16); //16= 16 16x16 blocks in a SB.       8=8search points

    // Initialize Alt-Ref parameters
    object_ptr->me_type                     = ME_CLOSE_LOOP;
    object_ptr->num_of_list_to_search       = 0;
    object_ptr->num_of_ref_pic_to_search[0] = 0;
    object_ptr->num_of_ref_pic_to_search[1] = 0;

    return EB_ErrorNone;
}
