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
#include "EbEncDecTasks.h"
#include "EbEncDecResults.h"
#include "EbCodingLoop.h"
#include "EbSvtAv1ErrorCodes.h"
#include "EbUtility.h"
#include "grainSynthesis.h"
//To fix warning C4013: 'svt_convert_16bit_to_8bit' undefined; assuming extern returning int
#include "common_dsp_rtcd.h"
#include "EbRateDistortionCost.h"
#include "EbPictureDecisionProcess.h"
#include "firstpass.h"
#include "EbPictureAnalysisProcess.h"

#define FC_SKIP_TX_SR_TH025 125 // Fast cost skip tx search threshold.
#define FC_SKIP_TX_SR_TH010 110 // Fast cost skip tx search threshold.
void copy_mv_rate(PictureControlSet *pcs, MdRateEstimationContext * dst_rate);
void svt_av1_cdef_search(EncDecContext *context_ptr, SequenceControlSet *scs_ptr,
                         PictureControlSet *pcs_ptr);

void av1_cdef_frame16bit(uint8_t is_16bit, SequenceControlSet *scs_ptr, PictureControlSet *pCs);

void svt_av1_add_film_grain(EbPictureBufferDesc *src, EbPictureBufferDesc *dst,
                            AomFilmGrain *film_grain_ptr);

void svt_av1_loop_restoration_save_boundary_lines(const Yv12BufferConfig *frame, Av1Common *cm,
                                                  int32_t after_cdef);
void svt_av1_loop_restoration_filter_frame(Yv12BufferConfig *frame, Av1Common *cm,
                                           int32_t optimized_lr);

static void enc_dec_context_dctor(EbPtr p) {
    EbThreadContext *thread_context_ptr = (EbThreadContext *)p;
    EncDecContext *  obj                = (EncDecContext *)thread_context_ptr->priv;
    EB_DELETE(obj->md_context);
    EB_DELETE(obj->residual_buffer);
    EB_DELETE(obj->transform_buffer);
    EB_DELETE(obj->inverse_quant_buffer);
    EB_DELETE(obj->input_sample16bit_buffer);
    if (obj->is_md_rate_estimation_ptr_owner) EB_FREE(obj->md_rate_estimation_ptr);
    EB_FREE_ARRAY(obj);
}

/******************************************************
 * Enc Dec Context Constructor
 ******************************************************/
EbErrorType enc_dec_context_ctor(EbThreadContext *  thread_context_ptr,
                                 const EbEncHandle *enc_handle_ptr, int index, int tasks_index,
                                 int demux_index)

{
    const EbSvtAv1EncConfiguration *static_config =
        &enc_handle_ptr->scs_instance_array[0]->scs_ptr->static_config;
    EbBool        is_16bit                 = (EbBool)(static_config->encoder_bit_depth > EB_8BIT);
    EbColorFormat color_format             = static_config->encoder_color_format;
    int8_t       enable_hbd_mode_decision = static_config->enable_hbd_mode_decision;

    EncDecContext *context_ptr;
    EB_CALLOC_ARRAY(context_ptr, 1);
    thread_context_ptr->priv  = context_ptr;
    thread_context_ptr->dctor = enc_dec_context_dctor;

    context_ptr->is_16bit     = is_16bit;
    context_ptr->color_format = color_format;

    // Input/Output System Resource Manager FIFOs
    context_ptr->mode_decision_input_fifo_ptr = svt_system_resource_get_consumer_fifo(
        enc_handle_ptr->enc_dec_tasks_resource_ptr, index);
    context_ptr->enc_dec_output_fifo_ptr = svt_system_resource_get_producer_fifo(
        enc_handle_ptr->enc_dec_results_resource_ptr, index);
    context_ptr->enc_dec_feedback_fifo_ptr = svt_system_resource_get_producer_fifo(
        enc_handle_ptr->enc_dec_tasks_resource_ptr, tasks_index);
    context_ptr->picture_demux_output_fifo_ptr = svt_system_resource_get_producer_fifo(
        enc_handle_ptr->picture_demux_results_resource_ptr, demux_index);

    // MD rate Estimation tables
    EB_MALLOC(context_ptr->md_rate_estimation_ptr, sizeof(MdRateEstimationContext));
    context_ptr->is_md_rate_estimation_ptr_owner = EB_TRUE;

    // Prediction Buffer
    context_ptr->input_sample16bit_buffer = NULL;
    if (is_16bit || static_config->is_16bit_pipeline)
        EB_NEW(context_ptr->input_sample16bit_buffer,
               svt_picture_buffer_desc_ctor,
               &(EbPictureBufferDescInitData){
                   .buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK,
                   .max_width          = SB_STRIDE_Y,
                   .max_height         = SB_STRIDE_Y,
                   .bit_depth          = EB_16BIT,
                   .left_padding       = 0,
                   .right_padding      = 0,
                   .top_padding        = 0,
                   .bot_padding        = 0,
                   .split_mode         = EB_FALSE,
                   .color_format       = color_format,
               });

    // Scratch Coeff Buffer
    EbPictureBufferDescInitData init_32bit_data = {
        .buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK,
        .max_width          = SB_STRIDE_Y,
        .max_height         = SB_STRIDE_Y,
        .bit_depth          = EB_32BIT,
        .color_format       = color_format,
        .left_padding       = 0,
        .right_padding      = 0,
        .top_padding        = 0,
        .bot_padding        = 0,
        .split_mode         = EB_FALSE,
    };

    EB_NEW(context_ptr->inverse_quant_buffer, svt_picture_buffer_desc_ctor, &init_32bit_data);
    EB_NEW(context_ptr->transform_buffer, svt_picture_buffer_desc_ctor, &init_32bit_data);
    EB_NEW(context_ptr->residual_buffer,
           svt_picture_buffer_desc_ctor,
           &(EbPictureBufferDescInitData){
               .buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK,
               .max_width          = SB_STRIDE_Y,
               .max_height         = SB_STRIDE_Y,
               .bit_depth          = EB_16BIT,
               .color_format       = color_format,
               .left_padding       = 0,
               .right_padding      = 0,
               .top_padding        = 0,
               .bot_padding        = 0,
               .split_mode         = EB_FALSE,
           });

    // Mode Decision Context
#if CLN_MD_CAND_BUFF
    EB_NEW(context_ptr->md_context,
           mode_decision_context_ctor,
           color_format,
           static_config->super_block_size,
           static_config->enc_mode,
           0,
           0,
           enable_hbd_mode_decision == DEFAULT ? 2 : enable_hbd_mode_decision ,
           static_config->screen_content_mode);
#else
    EB_NEW(context_ptr->md_context,
           mode_decision_context_ctor,
           color_format,
           static_config->super_block_size,
           0,
           0,
           enable_hbd_mode_decision == DEFAULT ? 2 : enable_hbd_mode_decision ,
           static_config->screen_content_mode);
#endif
    if (enable_hbd_mode_decision)
        context_ptr->md_context->input_sample16bit_buffer = context_ptr->input_sample16bit_buffer;

    context_ptr->md_context->enc_dec_context_ptr = context_ptr;

    return EB_ErrorNone;
}

/**************************************************
 * Reset Segmentation Map
 *************************************************/
static void reset_segmentation_map(SegmentationNeighborMap *segmentation_map) {
    if (segmentation_map->data != NULL)
        EB_MEMSET(segmentation_map->data, ~0, segmentation_map->map_size);
}

/**************************************************
 * Reset Mode Decision Neighbor Arrays
 *************************************************/
static void reset_encode_pass_neighbor_arrays(PictureControlSet *pcs_ptr, uint16_t tile_idx) {
    neighbor_array_unit_reset(pcs_ptr->ep_intra_luma_mode_neighbor_array[tile_idx]);
    neighbor_array_unit_reset(pcs_ptr->ep_intra_chroma_mode_neighbor_array[tile_idx]);
    neighbor_array_unit_reset(pcs_ptr->ep_mv_neighbor_array[tile_idx]);
    neighbor_array_unit_reset(pcs_ptr->ep_skip_flag_neighbor_array[tile_idx]);
    neighbor_array_unit_reset(pcs_ptr->ep_mode_type_neighbor_array[tile_idx]);
    neighbor_array_unit_reset(pcs_ptr->ep_luma_recon_neighbor_array[tile_idx]);
    neighbor_array_unit_reset(pcs_ptr->ep_cb_recon_neighbor_array[tile_idx]);
    neighbor_array_unit_reset(pcs_ptr->ep_cr_recon_neighbor_array[tile_idx]);
    neighbor_array_unit_reset(pcs_ptr->ep_luma_dc_sign_level_coeff_neighbor_array[tile_idx]);
    neighbor_array_unit_reset(pcs_ptr->ep_cb_dc_sign_level_coeff_neighbor_array[tile_idx]);
    neighbor_array_unit_reset(pcs_ptr->ep_cr_dc_sign_level_coeff_neighbor_array[tile_idx]);
    neighbor_array_unit_reset(pcs_ptr->ep_partition_context_neighbor_array[tile_idx]);
    // TODO(Joel): 8-bit ep_luma_recon_neighbor_array (Cb,Cr) when is_16bit==0?
    EbBool is_16bit =
        (EbBool)(pcs_ptr->parent_pcs_ptr->scs_ptr->static_config.encoder_bit_depth > EB_8BIT);
    if (is_16bit || pcs_ptr->parent_pcs_ptr->scs_ptr->static_config.is_16bit_pipeline) {
        neighbor_array_unit_reset(pcs_ptr->ep_luma_recon_neighbor_array16bit[tile_idx]);
        neighbor_array_unit_reset(pcs_ptr->ep_cb_recon_neighbor_array16bit[tile_idx]);
        neighbor_array_unit_reset(pcs_ptr->ep_cr_recon_neighbor_array16bit[tile_idx]);
    }
    return;
}

/**************************************************
 * Reset Coding Loop
 **************************************************/
static void reset_enc_dec(EncDecContext *context_ptr, PictureControlSet *pcs_ptr,
                          SequenceControlSet *scs_ptr, uint32_t segment_index) {
    context_ptr->is_16bit = (EbBool)(scs_ptr->static_config.encoder_bit_depth > EB_8BIT) || (EbBool)(scs_ptr->static_config.is_16bit_pipeline);
    context_ptr->bit_depth = scs_ptr->static_config.encoder_bit_depth;
    uint16_t tile_group_idx = context_ptr->tile_group_index;
    (*av1_lambda_assignment_function_table[pcs_ptr->parent_pcs_ptr->pred_structure])(
        pcs_ptr,
        &context_ptr->pic_fast_lambda[EB_8_BIT_MD],
        &context_ptr->pic_full_lambda[EB_8_BIT_MD],
        8,
        pcs_ptr->parent_pcs_ptr->frm_hdr.quantization_params.base_q_idx,
        EB_TRUE);

    (*av1_lambda_assignment_function_table[pcs_ptr->parent_pcs_ptr->pred_structure])(
        pcs_ptr,
        &context_ptr->pic_fast_lambda[EB_10_BIT_MD],
        &context_ptr->pic_full_lambda[EB_10_BIT_MD],
        10,
        pcs_ptr->parent_pcs_ptr->frm_hdr.quantization_params.base_q_idx,
        EB_TRUE);
    // Reset MD rate Estimation table to initial values by copying from md_rate_estimation_array
    if (context_ptr->is_md_rate_estimation_ptr_owner) {
        EB_FREE(context_ptr->md_rate_estimation_ptr);
        context_ptr->is_md_rate_estimation_ptr_owner = EB_FALSE;
    }
    context_ptr->md_rate_estimation_ptr = pcs_ptr->md_rate_estimation_array;
    if (segment_index == 0) {
        if (context_ptr->tile_group_index == 0) {
            reset_segmentation_map(pcs_ptr->segmentation_neighbor_map);
        }

        for (uint16_t r =
                 pcs_ptr->parent_pcs_ptr->tile_group_info[tile_group_idx].tile_group_tile_start_y;
             r < pcs_ptr->parent_pcs_ptr->tile_group_info[tile_group_idx].tile_group_tile_end_y;
             r++) {
            for (uint16_t c = pcs_ptr->parent_pcs_ptr->tile_group_info[tile_group_idx]
                                  .tile_group_tile_start_x;
                 c < pcs_ptr->parent_pcs_ptr->tile_group_info[tile_group_idx].tile_group_tile_end_x;
                 c++) {
                uint16_t tile_idx = c + r * pcs_ptr->parent_pcs_ptr->av1_cm->tiles_info.tile_cols;
                reset_encode_pass_neighbor_arrays(pcs_ptr, tile_idx);
            }
        }
    }

    return;
}

/******************************************************
 * Update MD Segments
 *
 * This function is responsible for synchronizing the
 *   processing of MD Segment-rows.
 *   In short, the function starts processing
 *   of MD segment-rows as soon as their inputs are available
 *   and the previous segment-row has completed.  At
 *   any given time, only one segment row per picture
 *   is being processed.
 *
 * The function has two functions:
 *
 * (1) Update the Segment Completion Mask which tracks
 *   which MD Segment inputs are available.
 *
 * (2) Increment the segment-row counter (current_row_idx)
 *   as the segment-rows are completed.
 *
 * Since there is the potentential for thread collusion,
 *   a MUTEX a used to protect the sensitive data and
 *   the execution flow is separated into two paths
 *
 * (A) Initial update.
 *  -Update the Completion Mask [see (1) above]
 *  -If the picture is not currently being processed,
 *     check to see if the next segment-row is available
 *     and start processing.
 * (b) Continued processing
 *  -Upon the completion of a segment-row, check
 *     to see if the next segment-row's inputs have
 *     become available and begin processing if so.
 *
 * On last important point is that the thread-safe
 *   code section is kept minimally short. The MUTEX
 *   should NOT be locked for the entire processing
 *   of the segment-row (b) as this would block other
 *   threads from performing an update (A).
 ******************************************************/
EbBool assign_enc_dec_segments(EncDecSegments *segmentPtr, uint16_t *segmentInOutIndex,
                               EncDecTasks *taskPtr, EbFifo *srmFifoPtr) {
    EbBool           continue_processing_flag = EB_FALSE;
    uint32_t row_segment_index = 0;
    uint32_t segment_index;
    uint32_t right_segment_index;
    uint32_t bottom_left_segment_index;

    int16_t feedback_row_index = -1;

    uint32_t self_assigned = EB_FALSE;

    //static FILE *trace = 0;
    //
    //if(trace == 0) {
    //    trace = fopen("seg-trace.txt","w");
    //}

    switch (taskPtr->input_type) {
    case ENCDEC_TASKS_MDC_INPUT:

        // The entire picture is provided by the MDC process, so
        //   no logic is necessary to clear input dependencies.
        // Reset enc_dec segments
        for (uint32_t row_index = 0; row_index < segmentPtr->segment_row_count; ++row_index) {
            segmentPtr->row_array[row_index].current_seg_index =
                segmentPtr->row_array[row_index].starting_seg_index;
        }

        // Start on Segment 0 immediately
        *segmentInOutIndex  = segmentPtr->row_array[0].current_seg_index;
        taskPtr->input_type = ENCDEC_TASKS_CONTINUE;
        ++segmentPtr->row_array[0].current_seg_index;
        continue_processing_flag = EB_TRUE;

        //fprintf(trace, "Start  Pic: %u Seg: %u\n",
        //    (unsigned) ((PictureControlSet*) taskPtr->pcs_wrapper_ptr->object_ptr)->picture_number,
        //    *segmentInOutIndex);

        break;

    case ENCDEC_TASKS_ENCDEC_INPUT:

        // Setup row_segment_index to release the in_progress token
        //row_segment_index = taskPtr->encDecSegmentRowArray[0];

        // Start on the assigned row immediately
        *segmentInOutIndex  = segmentPtr->row_array[taskPtr->enc_dec_segment_row].current_seg_index;
        taskPtr->input_type = ENCDEC_TASKS_CONTINUE;
        ++segmentPtr->row_array[taskPtr->enc_dec_segment_row].current_seg_index;
        continue_processing_flag = EB_TRUE;

        //fprintf(trace, "Start  Pic: %u Seg: %u\n",
        //    (unsigned) ((PictureControlSet*) taskPtr->pcs_wrapper_ptr->object_ptr)->picture_number,
        //    *segmentInOutIndex);

        break;

    case ENCDEC_TASKS_CONTINUE:

        // Update the Dependency List for Right and Bottom Neighbors
        segment_index     = *segmentInOutIndex;
        row_segment_index = segment_index / segmentPtr->segment_band_count;

        right_segment_index       = segment_index + 1;
        bottom_left_segment_index = segment_index + segmentPtr->segment_band_count;

        // Right Neighbor
        if (segment_index < segmentPtr->row_array[row_segment_index].ending_seg_index) {
            svt_block_on_mutex(segmentPtr->row_array[row_segment_index].assignment_mutex);

            --segmentPtr->dep_map.dependency_map[right_segment_index];

            if (segmentPtr->dep_map.dependency_map[right_segment_index] == 0) {
                *segmentInOutIndex = segmentPtr->row_array[row_segment_index].current_seg_index;
                ++segmentPtr->row_array[row_segment_index].current_seg_index;
                self_assigned            = EB_TRUE;
                continue_processing_flag = EB_TRUE;

                //fprintf(trace, "Start  Pic: %u Seg: %u\n",
                //    (unsigned) ((PictureControlSet*) taskPtr->pcs_wrapper_ptr->object_ptr)->picture_number,
                //    *segmentInOutIndex);
            }

            svt_release_mutex(segmentPtr->row_array[row_segment_index].assignment_mutex);
        }

        // Bottom-left Neighbor
        if (row_segment_index < segmentPtr->segment_row_count - 1 &&
            bottom_left_segment_index >=
                segmentPtr->row_array[row_segment_index + 1].starting_seg_index) {
            svt_block_on_mutex(segmentPtr->row_array[row_segment_index + 1].assignment_mutex);

            --segmentPtr->dep_map.dependency_map[bottom_left_segment_index];

            if (segmentPtr->dep_map.dependency_map[bottom_left_segment_index] == 0) {
                if (self_assigned == EB_TRUE)
                    feedback_row_index = (int16_t)row_segment_index + 1;
                else {
                    *segmentInOutIndex =
                        segmentPtr->row_array[row_segment_index + 1].current_seg_index;
                    ++segmentPtr->row_array[row_segment_index + 1].current_seg_index;
                    continue_processing_flag = EB_TRUE;

                    //fprintf(trace, "Start  Pic: %u Seg: %u\n",
                    //    (unsigned) ((PictureControlSet*) taskPtr->pcs_wrapper_ptr->object_ptr)->picture_number,
                    //    *segmentInOutIndex);
                }
            }
            svt_release_mutex(segmentPtr->row_array[row_segment_index + 1].assignment_mutex);
        }

        if (feedback_row_index > 0) {
            EbObjectWrapper *wrapper_ptr;
            svt_get_empty_object(srmFifoPtr, &wrapper_ptr);
            EncDecTasks *    feedback_task_ptr     = (EncDecTasks *)wrapper_ptr->object_ptr;
            feedback_task_ptr->input_type          = ENCDEC_TASKS_ENCDEC_INPUT;
            feedback_task_ptr->enc_dec_segment_row = feedback_row_index;
            feedback_task_ptr->pcs_wrapper_ptr     = taskPtr->pcs_wrapper_ptr;
            feedback_task_ptr->tile_group_index = taskPtr->tile_group_index;
            svt_post_full_object(wrapper_ptr);
        }

        break;

    default: break;
    }

    return continue_processing_flag;
}
void recon_output(PictureControlSet *pcs_ptr, SequenceControlSet *scs_ptr) {
    EncodeContext *     encode_context_ptr = scs_ptr->encode_context_ptr;
    // The totalNumberOfReconFrames counter has to be write/read protected as
    //   it is used to determine the end of the stream.  If it is not protected
    //   the encoder might not properly terminate.
    svt_block_on_mutex(encode_context_ptr->total_number_of_recon_frame_mutex);

    if (!pcs_ptr->parent_pcs_ptr->is_alt_ref) {
        EbBool           is_16bit = (scs_ptr->static_config.encoder_bit_depth > EB_8BIT);
        EbObjectWrapper *output_recon_wrapper_ptr;
        // Get Recon Buffer
        svt_get_empty_object(scs_ptr->encode_context_ptr->recon_output_fifo_ptr,
                             &output_recon_wrapper_ptr);
        EbBufferHeaderType *output_recon_ptr = (EbBufferHeaderType *)output_recon_wrapper_ptr->object_ptr;
        output_recon_ptr->flags = 0;

        // START READ/WRITE PROTECTED SECTION
        if (encode_context_ptr->total_number_of_recon_frames ==
            encode_context_ptr->terminating_picture_number)
            output_recon_ptr->flags = EB_BUFFERFLAG_EOS;

        encode_context_ptr->total_number_of_recon_frames++;

        // STOP READ/WRITE PROTECTED SECTION
        output_recon_ptr->n_filled_len = 0;

        // Copy the Reconstructed Picture to the Output Recon Buffer
        {
            uint32_t sample_total_count;
            uint8_t *recon_read_ptr;
            uint8_t *recon_write_ptr;

            EbPictureBufferDesc *recon_ptr;
            {
                if (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag == EB_TRUE)
                    recon_ptr = is_16bit ? ((EbReferenceObject *)pcs_ptr->parent_pcs_ptr
                                                ->reference_picture_wrapper_ptr->object_ptr)
                                               ->reference_picture16bit
                                         : ((EbReferenceObject *)pcs_ptr->parent_pcs_ptr
                                                ->reference_picture_wrapper_ptr->object_ptr)
                                               ->reference_picture;
                else {
                    if (is_16bit)
#if CLN_STRUCT
                        recon_ptr = pcs_ptr->parent_pcs_ptr->enc_dec_ptr->recon_picture16bit_ptr;
                    else
                        recon_ptr = pcs_ptr->parent_pcs_ptr->enc_dec_ptr->recon_picture_ptr;
#else
                        recon_ptr = pcs_ptr->recon_picture16bit_ptr;
                    else
                        recon_ptr = pcs_ptr->recon_picture_ptr;
#endif
                }
            }

            // FGN: Create a buffer if needed, copy the reconstructed picture and run the film grain synthesis algorithm

            if (scs_ptr->seq_header.film_grain_params_present) {
                EbPictureBufferDesc *intermediate_buffer_ptr;
                {
                    if (is_16bit)
                        intermediate_buffer_ptr = pcs_ptr->film_grain_picture16bit_ptr;
                    else
                        intermediate_buffer_ptr = pcs_ptr->film_grain_picture_ptr;
                }

                AomFilmGrain *film_grain_ptr;

                if (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag == EB_TRUE)
                    film_grain_ptr =
                        &((EbReferenceObject *)
                              pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
                             ->film_grain_params;
                else
                    film_grain_ptr = &pcs_ptr->parent_pcs_ptr->frm_hdr.film_grain_params;

                svt_av1_add_film_grain(recon_ptr, intermediate_buffer_ptr, film_grain_ptr);
                recon_ptr = intermediate_buffer_ptr;
            }

            // End running the film grain
            // Y Recon Samples
            sample_total_count = ((recon_ptr->max_width - scs_ptr->max_input_pad_right) *
                                  (recon_ptr->max_height - scs_ptr->max_input_pad_bottom))
                                 << is_16bit;
            recon_read_ptr = recon_ptr->buffer_y +
                             (recon_ptr->origin_y << is_16bit) * recon_ptr->stride_y +
                             (recon_ptr->origin_x << is_16bit);
            recon_write_ptr = &(output_recon_ptr->p_buffer[output_recon_ptr->n_filled_len]);

            CHECK_REPORT_ERROR((output_recon_ptr->n_filled_len + sample_total_count <=
                                output_recon_ptr->n_alloc_len),
                               encode_context_ptr->app_callback_ptr,
                               EB_ENC_ROB_OF_ERROR);

            // Initialize Y recon buffer
            picture_copy_kernel(recon_read_ptr,
                                recon_ptr->stride_y,
                                recon_write_ptr,
                                recon_ptr->max_width - scs_ptr->max_input_pad_right,
                                recon_ptr->width - scs_ptr->pad_right,
                                recon_ptr->height - scs_ptr->pad_bottom,
                                1 << is_16bit);

            output_recon_ptr->n_filled_len += sample_total_count;

            // U Recon Samples
            sample_total_count = ((recon_ptr->max_width - scs_ptr->max_input_pad_right) *
                                      (recon_ptr->max_height - scs_ptr->max_input_pad_bottom) >>
                                  2)
                                 << is_16bit;
            recon_read_ptr = recon_ptr->buffer_cb +
                             ((recon_ptr->origin_y << is_16bit) >> 1) * recon_ptr->stride_cb +
                             ((recon_ptr->origin_x << is_16bit) >> 1);
            recon_write_ptr = &(output_recon_ptr->p_buffer[output_recon_ptr->n_filled_len]);

            CHECK_REPORT_ERROR((output_recon_ptr->n_filled_len + sample_total_count <=
                                output_recon_ptr->n_alloc_len),
                               encode_context_ptr->app_callback_ptr,
                               EB_ENC_ROB_OF_ERROR);

            // Initialize U recon buffer
            picture_copy_kernel(recon_read_ptr,
                                recon_ptr->stride_cb,
                                recon_write_ptr,
                                (recon_ptr->max_width - scs_ptr->max_input_pad_right) >> 1,
                                (recon_ptr->width - scs_ptr->pad_right) >> 1,
                                (recon_ptr->height - scs_ptr->pad_bottom) >> 1,
                                1 << is_16bit);
            output_recon_ptr->n_filled_len += sample_total_count;

            // V Recon Samples
            sample_total_count = ((recon_ptr->max_width - scs_ptr->max_input_pad_right) *
                                      (recon_ptr->max_height - scs_ptr->max_input_pad_bottom) >>
                                  2)
                                 << is_16bit;
            recon_read_ptr = recon_ptr->buffer_cr +
                             ((recon_ptr->origin_y << is_16bit) >> 1) * recon_ptr->stride_cr +
                             ((recon_ptr->origin_x << is_16bit) >> 1);
            recon_write_ptr = &(output_recon_ptr->p_buffer[output_recon_ptr->n_filled_len]);

            CHECK_REPORT_ERROR((output_recon_ptr->n_filled_len + sample_total_count <=
                                output_recon_ptr->n_alloc_len),
                               encode_context_ptr->app_callback_ptr,
                               EB_ENC_ROB_OF_ERROR);

            // Initialize V recon buffer

            picture_copy_kernel(recon_read_ptr,
                                recon_ptr->stride_cr,
                                recon_write_ptr,
                                (recon_ptr->max_width - scs_ptr->max_input_pad_right) >> 1,
                                (recon_ptr->width - scs_ptr->pad_right) >> 1,
                                (recon_ptr->height - scs_ptr->pad_bottom) >> 1,
                                1 << is_16bit);
            output_recon_ptr->n_filled_len += sample_total_count;
            output_recon_ptr->pts = pcs_ptr->picture_number;
        }

        // Post the Recon object
        svt_post_full_object(output_recon_wrapper_ptr);
    } else {
        // Overlay and altref have 1 recon only, which is from overlay pictures. So the recon of the alt_ref is not sent to the application.
        // However, to hanlde the end of sequence properly, total_number_of_recon_frames is increamented
        encode_context_ptr->total_number_of_recon_frames++;
    }
    svt_release_mutex(encode_context_ptr->total_number_of_recon_frame_mutex);
}

//************************************/
// Calculate Frame SSIM
/************************************/

void aom_ssim_parms_8x8_c(const uint8_t *s, int sp, const uint8_t *r, int rp,
                          uint32_t *sum_s, uint32_t *sum_r, uint32_t *sum_sq_s,
                          uint32_t *sum_sq_r, uint32_t *sum_sxr) {
  int i, j;
  for (i = 0; i < 8; i++, s += sp, r += rp) {
    for (j = 0; j < 8; j++) {
      *sum_s += s[j];
      *sum_r += r[j];
      *sum_sq_s += s[j] * s[j];
      *sum_sq_r += r[j] * r[j];
      *sum_sxr += s[j] * r[j];
    }
  }
}

void aom_highbd_ssim_parms_8x8_c(const uint8_t *s, int sp, const uint8_t *sinc, int spinc, const uint16_t *r,
                                 int rp, uint32_t *sum_s, uint32_t *sum_r,
                                 uint32_t *sum_sq_s, uint32_t *sum_sq_r,
                                 uint32_t *sum_sxr) {
  int i, j;
  uint32_t ss;
  for (i = 0; i < 8; i++, s += sp, sinc += spinc, r += rp) {
    for (j = 0; j < 8; j++) {
      ss = (int64_t)(s[j] << 2) + ((sinc[j]>>6)&0x3);
      *sum_s += ss;
      *sum_r += r[j];
      *sum_sq_s += ss * ss;
      *sum_sq_r += r[j] * r[j];
      *sum_sxr += ss * r[j];
    }
  }
}

static const int64_t cc1 = 26634;        // (64^2*(.01*255)^2
static const int64_t cc2 = 239708;       // (64^2*(.03*255)^2
static const int64_t cc1_10 = 428658;    // (64^2*(.01*1023)^2
static const int64_t cc2_10 = 3857925;   // (64^2*(.03*1023)^2
static const int64_t cc1_12 = 6868593;   // (64^2*(.01*4095)^2
static const int64_t cc2_12 = 61817334;  // (64^2*(.03*4095)^2

static double similarity(uint32_t sum_s, uint32_t sum_r, uint32_t sum_sq_s,
                         uint32_t sum_sq_r, uint32_t sum_sxr, int count,
                         uint32_t bd) {
  double ssim_n, ssim_d;
  int64_t c1, c2;

  if (bd == 8) {
    // scale the constants by number of pixels
    c1 = (cc1 * count * count) >> 12;
    c2 = (cc2 * count * count) >> 12;
  } else if (bd == 10) {
    c1 = (cc1_10 * count * count) >> 12;
    c2 = (cc2_10 * count * count) >> 12;
  } else if (bd == 12) {
    c1 = (cc1_12 * count * count) >> 12;
    c2 = (cc2_12 * count * count) >> 12;
  } else {
    c1 = c2 = 0;
    assert(0);
  }

  ssim_n = (2.0 * sum_s * sum_r + c1) *
           (2.0 * count * sum_sxr - 2.0 * sum_s * sum_r + c2);

  ssim_d = ((double)sum_s * sum_s + (double)sum_r * sum_r + c1) *
           ((double)count * sum_sq_s - (double)sum_s * sum_s +
            (double)count * sum_sq_r - (double)sum_r * sum_r + c2);

  return ssim_n / ssim_d;
}

static double ssim_8x8(const uint8_t *s, int sp, const uint8_t *r, int rp) {
  uint32_t sum_s = 0, sum_r = 0, sum_sq_s = 0, sum_sq_r = 0, sum_sxr = 0;
  aom_ssim_parms_8x8_c(s, sp, r, rp, &sum_s, &sum_r, &sum_sq_s, &sum_sq_r, &sum_sxr);
  return similarity(sum_s, sum_r, sum_sq_s, sum_sq_r, sum_sxr, 64, 8);
}

static double highbd_ssim_8x8(const uint8_t *s, int sp, const uint8_t *sinc, int spinc, const uint16_t *r,
                              int rp, uint32_t bd, uint32_t shift) {
  uint32_t sum_s = 0, sum_r = 0, sum_sq_s = 0, sum_sq_r = 0, sum_sxr = 0;
  aom_highbd_ssim_parms_8x8_c(s, sp, sinc, spinc, r, rp, &sum_s, &sum_r, &sum_sq_s, &sum_sq_r, &sum_sxr);
  return similarity(sum_s >> shift, sum_r >> shift, sum_sq_s >> (2 * shift),
                    sum_sq_r >> (2 * shift), sum_sxr >> (2 * shift), 64, bd);
}

// We are using a 8x8 moving window with starting location of each 8x8 window
// on the 4x4 pixel grid. Such arrangement allows the windows to overlap
// block boundaries to penalize blocking artifacts.
static double aom_ssim2(const uint8_t *img1, int stride_img1,
                        const uint8_t *img2, int stride_img2,
                        int width, int height) {
    int i, j;
    int samples = 0;
    double ssim_total = 0;

    // sample point start with each 4x4 location
    for (i = 0; i <= height - 8;
        i += 4, img1 += stride_img1 * 4, img2 += stride_img2 * 4) {
        for (j = 0; j <= width - 8; j += 4) {
            double v = ssim_8x8(img1 + j, stride_img1, img2 + j, stride_img2);
            ssim_total += v;
            samples++;
        }
    }
    assert(samples > 0);
    ssim_total /= samples;
    return ssim_total;
}

static double aom_highbd_ssim2(const uint8_t *img1, int stride_img1,
                               const uint8_t *img1inc, int stride_img1inc,
                               const uint16_t *img2, int stride_img2,
                               int width, int height, uint32_t bd, uint32_t shift) {
  int i, j;
  int samples = 0;
  double ssim_total = 0;

  // sample point start with each 4x4 location
  for (i = 0; i <= height - 8;
       i += 4, img1 += stride_img1 * 4, img1inc += stride_img1inc * 4, img2 += stride_img2 * 4) {
    for (j = 0; j <= width - 8; j += 4) {
      double v = highbd_ssim_8x8((img1 + j), stride_img1,
                                 (img1inc + j), stride_img1inc,
                                 (img2 + j), stride_img2, bd,
                                 shift);
      ssim_total += v;
      samples++;
    }
  }
  assert(samples > 0);
  ssim_total /= samples;
  return ssim_total;
}

void ssim_calculations(PictureControlSet *pcs_ptr, SequenceControlSet *scs_ptr, EbBool free_memory) {
    EbBool is_16bit = (scs_ptr->static_config.encoder_bit_depth > EB_8BIT);

    const uint32_t ss_x = scs_ptr->subsampling_x;
    const uint32_t ss_y = scs_ptr->subsampling_y;

    if (!is_16bit) {
        EbPictureBufferDesc *recon_ptr;

        if (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag == EB_TRUE)
            recon_ptr = ((EbReferenceObject*)pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)->reference_picture;
        else
#if CLN_STRUCT
            recon_ptr = pcs_ptr->parent_pcs_ptr->enc_dec_ptr->recon_picture_ptr;
#else
            recon_ptr = pcs_ptr->recon_picture_ptr;
#endif

        EbPictureBufferDesc *input_picture_ptr = (EbPictureBufferDesc*)pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr;

        EbByte  input_buffer;
        EbByte  recon_coeff_buffer;

        EbByte buffer_y;
        EbByte buffer_cb;
        EbByte buffer_cr;

        double luma_ssim = 0.0;
        double cb_ssim = 0.0;
        double cr_ssim = 0.0;

        // if current source picture was temporally filtered, use an alternative buffer which stores
        // the original source picture
        if(pcs_ptr->parent_pcs_ptr->temporal_filtering_on == EB_TRUE){
            buffer_y = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_ptr[0];
            buffer_cb = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_ptr[1];
            buffer_cr = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_ptr[2];
        }
        else {
            buffer_y = input_picture_ptr->buffer_y;
            buffer_cb = input_picture_ptr->buffer_cb;
            buffer_cr = input_picture_ptr->buffer_cr;
        }

        recon_coeff_buffer = &((recon_ptr->buffer_y)[recon_ptr->origin_x + recon_ptr->origin_y * recon_ptr->stride_y]);
        input_buffer = &(buffer_y[input_picture_ptr->origin_x + input_picture_ptr->origin_y * input_picture_ptr->stride_y]);
        luma_ssim = aom_ssim2(input_buffer, input_picture_ptr->stride_y, recon_coeff_buffer, recon_ptr->stride_y,
                              scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height);

        recon_coeff_buffer = &((recon_ptr->buffer_cb)[recon_ptr->origin_x / 2 + recon_ptr->origin_y / 2 * recon_ptr->stride_cb]);
        input_buffer = &(buffer_cb[input_picture_ptr->origin_x / 2 + input_picture_ptr->origin_y / 2 * input_picture_ptr->stride_cb]);
        cb_ssim = aom_ssim2(input_buffer, input_picture_ptr->stride_cb, recon_coeff_buffer, recon_ptr->stride_cb,
                            scs_ptr->chroma_width, scs_ptr->chroma_height);

        recon_coeff_buffer = &((recon_ptr->buffer_cr)[recon_ptr->origin_x / 2 + recon_ptr->origin_y / 2 * recon_ptr->stride_cr]);
        input_buffer = &(buffer_cr[input_picture_ptr->origin_x / 2 + input_picture_ptr->origin_y / 2 * input_picture_ptr->stride_cr]);
        cr_ssim = aom_ssim2(input_buffer, input_picture_ptr->stride_cr, recon_coeff_buffer, recon_ptr->stride_cr,
                            scs_ptr->chroma_width, scs_ptr->chroma_height);

        pcs_ptr->parent_pcs_ptr->luma_ssim = luma_ssim;
        pcs_ptr->parent_pcs_ptr->cb_ssim = cb_ssim;
        pcs_ptr->parent_pcs_ptr->cr_ssim = cr_ssim;

        if (free_memory && pcs_ptr->parent_pcs_ptr->temporal_filtering_on == EB_TRUE) {
            EB_FREE_ARRAY(buffer_y);
            EB_FREE_ARRAY(buffer_cb);
            EB_FREE_ARRAY(buffer_cr);
        }
    }
    else {
      EbPictureBufferDesc *recon_ptr;

        if (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag == EB_TRUE)
            recon_ptr = ((EbReferenceObject*)pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)->reference_picture16bit;
        else
#if CLN_STRUCT
            recon_ptr = pcs_ptr->parent_pcs_ptr->enc_dec_ptr->recon_picture16bit_ptr;
#else
            recon_ptr = pcs_ptr->recon_picture16bit_ptr;
#endif
        EbPictureBufferDesc *input_picture_ptr = (EbPictureBufferDesc*)pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr;

        EbByte    input_buffer;
        uint16_t *recon_coeff_buffer;

        double luma_ssim = 0.0;
        double cb_ssim = 0.0;
        double cr_ssim = 0.0;

        if (scs_ptr->static_config.ten_bit_format == 1) {

            /* SSIM calculation for compressed 10-bit format has not been verified and debugged,
               since this format is not supported elsewhere in this version. See verify_settings(),
               which exits with an error if compressed 10-bit format is enabled. To avoid
               extra complexity of unpacking into a temporary buffer, or having to write
               new core SSIM functions, we ignore the two least signifcant bits in this
               case, and set these to zero. One test shows a difference in SSIM
               of 0.00085 setting the two least significant bits to zero. */

            const uint32_t luma_width        = input_picture_ptr->width - scs_ptr->max_input_pad_right;
            const uint32_t luma_height       = input_picture_ptr->height - scs_ptr->max_input_pad_bottom;
            const uint32_t chroma_width      = luma_width >> ss_x;
            const uint32_t pic_width_in_sb   = (luma_width + 64 - 1) / 64;
            const uint32_t pic_height_in_sb  = (luma_height + 64 - 1) / 64;
            const uint32_t chroma_height     = luma_height >> ss_y;
            uint32_t       sb_num_in_height, sb_num_in_width, bd, shift;
            uint8_t        zero_buffer[64*64];

            bd = 10;
            shift = 0 ; // both input and output are 10 bit (bitdepth - input_bd)
            memset(&zero_buffer[0], 0, sizeof(uint8_t)*64*64);

            EbByte input_buffer_org =
                &((input_picture_ptr
                       ->buffer_y)[input_picture_ptr->origin_x +
                                   input_picture_ptr->origin_y * input_picture_ptr->stride_y]);
            uint16_t *recon_buffer_org = (uint16_t *)(&(
                (recon_ptr->buffer_y)[(recon_ptr->origin_x << is_16bit) +
                                      (recon_ptr->origin_y << is_16bit) * recon_ptr->stride_y]));
            ;

            EbByte input_buffer_org_u = &(
                (input_picture_ptr
                     ->buffer_cb)[input_picture_ptr->origin_x / 2 +
                                  input_picture_ptr->origin_y / 2 * input_picture_ptr->stride_cb]);
            ;
            uint16_t *recon_buffer_org_u = recon_coeff_buffer =
                (uint16_t *)(&((recon_ptr->buffer_cb)[(recon_ptr->origin_x << is_16bit) / 2 +
                                                      (recon_ptr->origin_y << is_16bit) / 2 *
                                                          recon_ptr->stride_cb]));
            ;

            EbByte input_buffer_org_v = &(
                (input_picture_ptr
                     ->buffer_cr)[input_picture_ptr->origin_x / 2 +
                                  input_picture_ptr->origin_y / 2 * input_picture_ptr->stride_cr]);
            ;
            uint16_t *recon_buffer_org_v = recon_coeff_buffer =
                (uint16_t *)(&((recon_ptr->buffer_cr)[(recon_ptr->origin_x << is_16bit) / 2 +
                                                      (recon_ptr->origin_y << is_16bit) / 2 *
                                                          recon_ptr->stride_cr]));
            ;

           for (sb_num_in_height = 0; sb_num_in_height < pic_height_in_sb; ++sb_num_in_height) {
                for (sb_num_in_width = 0; sb_num_in_width < pic_width_in_sb; ++sb_num_in_width) {
                    uint32_t tb_origin_x = sb_num_in_width * 64;
                    uint32_t tb_origin_y = sb_num_in_height * 64;
                    uint32_t sb_width =
                        (luma_width - tb_origin_x) < 64 ? (luma_width - tb_origin_x) : 64;
                    uint32_t sb_height =
                        (luma_height - tb_origin_y) < 64 ? (luma_height - tb_origin_y) : 64;

                    input_buffer =
                        input_buffer_org + tb_origin_y * input_picture_ptr->stride_y + tb_origin_x;
                    recon_coeff_buffer =
                        recon_buffer_org + tb_origin_y * recon_ptr->stride_y + tb_origin_x;

                    luma_ssim += aom_highbd_ssim2(input_buffer, input_picture_ptr->stride_y, &zero_buffer[0], 64,
                                                  recon_coeff_buffer, recon_ptr->stride_y, sb_width, sb_height, bd, shift);

                    //U+V
                    tb_origin_x = sb_num_in_width * 32;
                    tb_origin_y = sb_num_in_height * 32;
                    sb_width =
                        (chroma_width - tb_origin_x) < 32 ? (chroma_width - tb_origin_x) : 32;
                    sb_height =
                        (chroma_height - tb_origin_y) < 32 ? (chroma_height - tb_origin_y) : 32;

                    input_buffer = input_buffer_org_u + tb_origin_y * input_picture_ptr->stride_cb +
                                   tb_origin_x;
                    recon_coeff_buffer =
                        recon_buffer_org_u + tb_origin_y * recon_ptr->stride_cb + tb_origin_x;

                    cb_ssim += aom_highbd_ssim2(input_buffer, input_picture_ptr->stride_cb, &zero_buffer[0], 64,
                                                recon_coeff_buffer, recon_ptr->stride_cb, sb_width, sb_height, bd, shift);

                    input_buffer = input_buffer_org_v + tb_origin_y * input_picture_ptr->stride_cr +
                                   tb_origin_x;
                    recon_coeff_buffer =
                        recon_buffer_org_v + tb_origin_y * recon_ptr->stride_cr + tb_origin_x;

                    cr_ssim += aom_highbd_ssim2(input_buffer, input_picture_ptr->stride_cr, &zero_buffer[0], 64,
                                                recon_coeff_buffer, recon_ptr->stride_cr, sb_width, sb_height, bd, shift);
                }
            }

            luma_ssim /= pic_height_in_sb * pic_width_in_sb;
            cb_ssim   /= pic_height_in_sb * pic_width_in_sb;
            cr_ssim   /= pic_height_in_sb * pic_width_in_sb;

            pcs_ptr->parent_pcs_ptr->luma_ssim = luma_ssim;
            pcs_ptr->parent_pcs_ptr->cb_ssim = cb_ssim;
            pcs_ptr->parent_pcs_ptr->cr_ssim = cr_ssim;
        }
        else {
            recon_coeff_buffer = (uint16_t*)(&((recon_ptr->buffer_y)[(recon_ptr->origin_x << is_16bit) + (recon_ptr->origin_y << is_16bit) * recon_ptr->stride_y]));

            // if current source picture was temporally filtered, use an alternative buffer which stores
            // the original source picture
            EbByte buffer_y, buffer_bit_inc_y;
            EbByte buffer_cb, buffer_bit_inc_cb;
            EbByte buffer_cr, buffer_bit_inc_cr;
            int bd, shift;

            if(pcs_ptr->parent_pcs_ptr->temporal_filtering_on == EB_TRUE){
                buffer_y = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_ptr[0];
                buffer_bit_inc_y = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_bit_inc_ptr[0];
                buffer_cb = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_ptr[1];
                buffer_bit_inc_cb = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_bit_inc_ptr[1];
                buffer_cr = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_ptr[2];
                buffer_bit_inc_cr = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_bit_inc_ptr[2];
            }else{
                buffer_y = input_picture_ptr->buffer_y;
                buffer_bit_inc_y = input_picture_ptr->buffer_bit_inc_y;
                buffer_cb = input_picture_ptr->buffer_cb;
                buffer_bit_inc_cb = input_picture_ptr->buffer_bit_inc_cb;
                buffer_cr = input_picture_ptr->buffer_cr;
                buffer_bit_inc_cr = input_picture_ptr->buffer_bit_inc_cr;
            }

            bd = 10;
            shift = 0 ; // both input and output are 10 bit (bitdepth - input_bd)

            input_buffer = &((buffer_y)[input_picture_ptr->origin_x + input_picture_ptr->origin_y * input_picture_ptr->stride_y]);
            EbByte input_buffer_bit_inc = &(
                (buffer_bit_inc_y)[input_picture_ptr->origin_x +
                                   input_picture_ptr->origin_y *
                                       input_picture_ptr->stride_bit_inc_y]);
            luma_ssim = aom_highbd_ssim2(input_buffer, input_picture_ptr->stride_y, input_buffer_bit_inc, input_picture_ptr->stride_bit_inc_y,
                                         recon_coeff_buffer, recon_ptr->stride_y, scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, bd, shift);

            recon_coeff_buffer = (uint16_t*)(&((recon_ptr->buffer_cb)[(recon_ptr->origin_x << is_16bit) / 2 + (recon_ptr->origin_y << is_16bit) / 2 * recon_ptr->stride_cb]));
            input_buffer = &((buffer_cb)[input_picture_ptr->origin_x / 2 + input_picture_ptr->origin_y / 2 * input_picture_ptr->stride_cb]);
            input_buffer_bit_inc = &((buffer_bit_inc_cb)[input_picture_ptr->origin_x / 2 + input_picture_ptr->origin_y / 2 * input_picture_ptr->stride_bit_inc_cb]);
            cb_ssim = aom_highbd_ssim2(input_buffer, input_picture_ptr->stride_cb, input_buffer_bit_inc, input_picture_ptr->stride_bit_inc_cb,
                                       recon_coeff_buffer, recon_ptr->stride_cb, scs_ptr->chroma_width, scs_ptr->chroma_height, bd, shift);

            recon_coeff_buffer = (uint16_t*)(&((recon_ptr->buffer_cr)[(recon_ptr->origin_x << is_16bit) / 2 + (recon_ptr->origin_y << is_16bit) / 2 * recon_ptr->stride_cr]));
            input_buffer = &((buffer_cr)[input_picture_ptr->origin_x / 2 + input_picture_ptr->origin_y / 2 * input_picture_ptr->stride_cr]);
            input_buffer_bit_inc = &((buffer_bit_inc_cr)[input_picture_ptr->origin_x / 2 + input_picture_ptr->origin_y / 2 * input_picture_ptr->stride_bit_inc_cr]);
            cr_ssim = aom_highbd_ssim2(input_buffer, input_picture_ptr->stride_cr, input_buffer_bit_inc, input_picture_ptr->stride_bit_inc_cr,
                                    recon_coeff_buffer, recon_ptr->stride_cr, scs_ptr->chroma_width, scs_ptr->chroma_height, bd, shift);

            pcs_ptr->parent_pcs_ptr->luma_ssim = luma_ssim;
            pcs_ptr->parent_pcs_ptr->cb_ssim = cb_ssim;
            pcs_ptr->parent_pcs_ptr->cr_ssim = cr_ssim;

            if (free_memory && pcs_ptr->parent_pcs_ptr->temporal_filtering_on == EB_TRUE) {
                EB_FREE_ARRAY(buffer_y);
                EB_FREE_ARRAY(buffer_cb);
                EB_FREE_ARRAY(buffer_cr);
                EB_FREE_ARRAY(buffer_bit_inc_y);
                EB_FREE_ARRAY(buffer_bit_inc_cb);
                EB_FREE_ARRAY(buffer_bit_inc_cr);
            }
        }
    }

}

void psnr_calculations(PictureControlSet *pcs_ptr, SequenceControlSet *scs_ptr, EbBool free_memory) {
    EbBool is_16bit = (scs_ptr->static_config.encoder_bit_depth > EB_8BIT);

    const uint32_t ss_x = scs_ptr->subsampling_x;
    const uint32_t ss_y = scs_ptr->subsampling_y;

    if (!is_16bit) {
        EbPictureBufferDesc *recon_ptr;

        if (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag == EB_TRUE)
            recon_ptr = ((EbReferenceObject *)
                             pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
                            ->reference_picture;
        else
#if CLN_STRUCT
            recon_ptr = pcs_ptr->parent_pcs_ptr->enc_dec_ptr->recon_picture_ptr;
#else
            recon_ptr = pcs_ptr->recon_picture_ptr;
#endif

        EbPictureBufferDesc *input_picture_ptr =
            (EbPictureBufferDesc *)pcs_ptr->parent_pcs_ptr->enhanced_unscaled_picture_ptr;

        uint64_t sse_total[3] = {0};
        uint64_t residual_distortion = 0;
        EbByte   input_buffer;
        EbByte   recon_coeff_buffer;

        EbByte buffer_y;
        EbByte buffer_cb;
        EbByte buffer_cr;

        // if current source picture was temporally filtered, use an alternative buffer which stores
        // the original source picture
        if (pcs_ptr->parent_pcs_ptr->temporal_filtering_on == EB_TRUE) {
            buffer_y  = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_ptr[0];
            buffer_cb = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_ptr[1];
            buffer_cr = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_ptr[2];
        } else {
            buffer_y  = input_picture_ptr->buffer_y;
            buffer_cb = input_picture_ptr->buffer_cb;
            buffer_cr = input_picture_ptr->buffer_cr;
        }

        recon_coeff_buffer = &(
            (recon_ptr->buffer_y)[recon_ptr->origin_x + recon_ptr->origin_y * recon_ptr->stride_y]);
        input_buffer = &(buffer_y[input_picture_ptr->origin_x +
                                  input_picture_ptr->origin_y * input_picture_ptr->stride_y]);

        residual_distortion = 0;

        for (int row_index = 0;
             row_index < input_picture_ptr->height - scs_ptr->max_input_pad_bottom;
             ++row_index) {
            for (int column_index = 0;
                 column_index < input_picture_ptr->width - scs_ptr->max_input_pad_right;
                 ++column_index) {
                residual_distortion += (int64_t)SQR((int64_t)(input_buffer[column_index]) -
                                                    (recon_coeff_buffer[column_index]));
            }

            input_buffer += input_picture_ptr->stride_y;
            recon_coeff_buffer += recon_ptr->stride_y;
        }

        sse_total[0] = residual_distortion;

        recon_coeff_buffer =
            &((recon_ptr->buffer_cb)[recon_ptr->origin_x / 2 +
                                     recon_ptr->origin_y / 2 * recon_ptr->stride_cb]);
        input_buffer = &(buffer_cb[input_picture_ptr->origin_x / 2 +
                                   input_picture_ptr->origin_y / 2 * input_picture_ptr->stride_cb]);

        residual_distortion = 0;
        for (int row_index = 0; row_index <
             (input_picture_ptr->height - scs_ptr->max_input_pad_bottom) >> ss_y;
             ++row_index) {
            for (int column_index = 0; column_index <
                 (input_picture_ptr->width - scs_ptr->max_input_pad_right) >> ss_x;
                 ++column_index) {
                residual_distortion += (int64_t)SQR((int64_t)(input_buffer[column_index]) -
                                                    (recon_coeff_buffer[column_index]));
            }

            input_buffer += input_picture_ptr->stride_cb;
            recon_coeff_buffer += recon_ptr->stride_cb;
        }

        sse_total[1] = residual_distortion;

        recon_coeff_buffer =
            &((recon_ptr->buffer_cr)[recon_ptr->origin_x / 2 +
                                     recon_ptr->origin_y / 2 * recon_ptr->stride_cr]);
        input_buffer        = &(buffer_cr[input_picture_ptr->origin_x / 2 +
                                   input_picture_ptr->origin_y / 2 * input_picture_ptr->stride_cr]);
        residual_distortion = 0;

        for (int row_index = 0; row_index <
             (input_picture_ptr->height - scs_ptr->max_input_pad_bottom) >> ss_y;
             ++row_index) {
            for (int column_index = 0; column_index <
                 (input_picture_ptr->width - scs_ptr->max_input_pad_right) >> ss_x;
                 ++column_index) {
                residual_distortion += (int64_t)SQR((int64_t)(input_buffer[column_index]) -
                                                    (recon_coeff_buffer[column_index]));
            }

            input_buffer += input_picture_ptr->stride_cr;
            recon_coeff_buffer += recon_ptr->stride_cr;
        }

        sse_total[2]                      = residual_distortion;
        pcs_ptr->parent_pcs_ptr->luma_sse = (uint32_t)sse_total[0];
        pcs_ptr->parent_pcs_ptr->cb_sse   = (uint32_t)sse_total[1];
        pcs_ptr->parent_pcs_ptr->cr_sse   = (uint32_t)sse_total[2];

        if(free_memory && pcs_ptr->parent_pcs_ptr->temporal_filtering_on == EB_TRUE) {
            EB_FREE_ARRAY(buffer_y);
            EB_FREE_ARRAY(buffer_cb);
            EB_FREE_ARRAY(buffer_cr);
        }
    }
    else {
        EbPictureBufferDesc *recon_ptr;

        if (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag == EB_TRUE)
            recon_ptr = ((EbReferenceObject *)
                             pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
                            ->reference_picture16bit;
        else
#if CLN_STRUCT
            recon_ptr = pcs_ptr->parent_pcs_ptr->enc_dec_ptr->recon_picture16bit_ptr;
#else
            recon_ptr = pcs_ptr->recon_picture16bit_ptr;
#endif
        EbPictureBufferDesc *input_picture_ptr =
            (EbPictureBufferDesc *)pcs_ptr->parent_pcs_ptr->enhanced_unscaled_picture_ptr;

        uint64_t  sse_total[3] = {0};
        uint64_t  residual_distortion = 0;
        EbByte    input_buffer;
        EbByte    input_buffer_bit_inc;
        uint16_t *recon_coeff_buffer;

        if (scs_ptr->static_config.ten_bit_format == 1) {
            const uint32_t luma_width        = input_picture_ptr->width - scs_ptr->max_input_pad_right;
            const uint32_t luma_height       = input_picture_ptr->height - scs_ptr->max_input_pad_bottom;
            const uint32_t chroma_width      = luma_width >> ss_x;
            const uint32_t pic_width_in_sb   = (luma_width + 64 - 1) / 64;
            const uint32_t pic_height_in_sb  = (luma_height + 64 - 1) / 64;
            const uint32_t luma_2bit_width   = luma_width / 4;
            const uint32_t chroma_height     = luma_height >> ss_y;
            const uint32_t chroma_2bit_width = chroma_width / 4;
            uint32_t       sb_num_in_height, sb_num_in_width;

            EbByte input_buffer_org =
                &((input_picture_ptr
                       ->buffer_y)[input_picture_ptr->origin_x +
                                   input_picture_ptr->origin_y * input_picture_ptr->stride_y]);
            uint16_t *recon_buffer_org = (uint16_t *)(&(
                (recon_ptr->buffer_y)[(recon_ptr->origin_x << is_16bit) +
                                      (recon_ptr->origin_y << is_16bit) * recon_ptr->stride_y]));
            ;

            EbByte input_buffer_org_u = &(
                (input_picture_ptr
                     ->buffer_cb)[input_picture_ptr->origin_x / 2 +
                                  input_picture_ptr->origin_y / 2 * input_picture_ptr->stride_cb]);
            ;
            uint16_t *recon_buffer_org_u = recon_coeff_buffer =
                (uint16_t *)(&((recon_ptr->buffer_cb)[(recon_ptr->origin_x << is_16bit) / 2 +
                                                      (recon_ptr->origin_y << is_16bit) / 2 *
                                                          recon_ptr->stride_cb]));
            ;

            EbByte input_buffer_org_v = &(
                (input_picture_ptr
                     ->buffer_cr)[input_picture_ptr->origin_x / 2 +
                                  input_picture_ptr->origin_y / 2 * input_picture_ptr->stride_cr]);
            ;
            uint16_t *recon_buffer_org_v = recon_coeff_buffer =
                (uint16_t *)(&((recon_ptr->buffer_cr)[(recon_ptr->origin_x << is_16bit) / 2 +
                                                      (recon_ptr->origin_y << is_16bit) / 2 *
                                                          recon_ptr->stride_cr]));
            ;

            residual_distortion            = 0;
            uint64_t residual_distortion_u = 0;
            uint64_t residual_distortion_v = 0;

            for (sb_num_in_height = 0; sb_num_in_height < pic_height_in_sb; ++sb_num_in_height) {
                for (sb_num_in_width = 0; sb_num_in_width < pic_width_in_sb; ++sb_num_in_width) {
                    uint32_t tb_origin_x = sb_num_in_width * 64;
                    uint32_t tb_origin_y = sb_num_in_height * 64;
                    uint32_t sb_width =
                        (luma_width - tb_origin_x) < 64 ? (luma_width - tb_origin_x) : 64;
                    uint32_t sb_height =
                        (luma_height - tb_origin_y) < 64 ? (luma_height - tb_origin_y) : 64;

                    input_buffer =
                        input_buffer_org + tb_origin_y * input_picture_ptr->stride_y + tb_origin_x;
                    input_buffer_bit_inc = input_picture_ptr->buffer_bit_inc_y +
                                           tb_origin_y * luma_2bit_width +
                                           (tb_origin_x / 4) * sb_height;
                    recon_coeff_buffer =
                        recon_buffer_org + tb_origin_y * recon_ptr->stride_y + tb_origin_x;

                    uint64_t j, k;
                    uint16_t out_pixel;
                    uint8_t  n_bit_pixel;
                    uint8_t  four_2bit_pels;
                    uint32_t inn_stride = sb_width / 4;

                    for (j = 0; j < sb_height; j++) {
                        for (k = 0; k < sb_width / 4; k++) {
                            four_2bit_pels = input_buffer_bit_inc[k + j * inn_stride];

                            n_bit_pixel = (four_2bit_pels >> 6) & 3;
                            out_pixel   = input_buffer[k * 4 + 0 + j * input_picture_ptr->stride_y]
                                        << 2;
                            out_pixel = out_pixel | n_bit_pixel;
                            residual_distortion += (int64_t)SQR(
                                (int64_t)out_pixel -
                                (int64_t)recon_coeff_buffer[k * 4 + 0 + j * recon_ptr->stride_y]);

                            n_bit_pixel = (four_2bit_pels >> 4) & 3;
                            out_pixel   = input_buffer[k * 4 + 1 + j * input_picture_ptr->stride_y]
                                        << 2;
                            out_pixel = out_pixel | n_bit_pixel;
                            residual_distortion += (int64_t)SQR(
                                (int64_t)out_pixel -
                                (int64_t)recon_coeff_buffer[k * 4 + 1 + j * recon_ptr->stride_y]);

                            n_bit_pixel = (four_2bit_pels >> 2) & 3;
                            out_pixel   = input_buffer[k * 4 + 2 + j * input_picture_ptr->stride_y]
                                        << 2;
                            out_pixel = out_pixel | n_bit_pixel;
                            residual_distortion += (int64_t)SQR(
                                (int64_t)out_pixel -
                                (int64_t)recon_coeff_buffer[k * 4 + 2 + j * recon_ptr->stride_y]);

                            n_bit_pixel = (four_2bit_pels >> 0) & 3;
                            out_pixel   = input_buffer[k * 4 + 3 + j * input_picture_ptr->stride_y]
                                        << 2;
                            out_pixel = out_pixel | n_bit_pixel;
                            residual_distortion += (int64_t)SQR(
                                (int64_t)out_pixel -
                                (int64_t)recon_coeff_buffer[k * 4 + 3 + j * recon_ptr->stride_y]);
                        }
                    }

                    //U+V

                    tb_origin_x = sb_num_in_width * 32;
                    tb_origin_y = sb_num_in_height * 32;
                    sb_width =
                        (chroma_width - tb_origin_x) < 32 ? (chroma_width - tb_origin_x) : 32;
                    sb_height =
                        (chroma_height - tb_origin_y) < 32 ? (chroma_height - tb_origin_y) : 32;

                    inn_stride = sb_width / 4;

                    input_buffer = input_buffer_org_u + tb_origin_y * input_picture_ptr->stride_cb +
                                   tb_origin_x;

                    input_buffer_bit_inc = input_picture_ptr->buffer_bit_inc_cb +
                                           tb_origin_y * chroma_2bit_width +
                                           (tb_origin_x / 4) * sb_height;

                    recon_coeff_buffer =
                        recon_buffer_org_u + tb_origin_y * recon_ptr->stride_cb + tb_origin_x;

                    for (j = 0; j < sb_height; j++) {
                        for (k = 0; k < sb_width / 4; k++) {
                            four_2bit_pels = input_buffer_bit_inc[k + j * inn_stride];

                            n_bit_pixel = (four_2bit_pels >> 6) & 3;
                            out_pixel   = input_buffer[k * 4 + 0 + j * input_picture_ptr->stride_cb]
                                        << 2;
                            out_pixel = out_pixel | n_bit_pixel;
                            residual_distortion_u += (int64_t)SQR(
                                (int64_t)out_pixel -
                                (int64_t)recon_coeff_buffer[k * 4 + 0 + j * recon_ptr->stride_cb]);

                            n_bit_pixel = (four_2bit_pels >> 4) & 3;
                            out_pixel   = input_buffer[k * 4 + 1 + j * input_picture_ptr->stride_cb]
                                        << 2;
                            out_pixel = out_pixel | n_bit_pixel;
                            residual_distortion_u += (int64_t)SQR(
                                (int64_t)out_pixel -
                                (int64_t)recon_coeff_buffer[k * 4 + 1 + j * recon_ptr->stride_cb]);

                            n_bit_pixel = (four_2bit_pels >> 2) & 3;
                            out_pixel   = input_buffer[k * 4 + 2 + j * input_picture_ptr->stride_cb]
                                        << 2;
                            out_pixel = out_pixel | n_bit_pixel;
                            residual_distortion_u += (int64_t)SQR(
                                (int64_t)out_pixel -
                                (int64_t)recon_coeff_buffer[k * 4 + 2 + j * recon_ptr->stride_cb]);

                            n_bit_pixel = (four_2bit_pels >> 0) & 3;
                            out_pixel   = input_buffer[k * 4 + 3 + j * input_picture_ptr->stride_cb]
                                        << 2;
                            out_pixel = out_pixel | n_bit_pixel;
                            residual_distortion_u += (int64_t)SQR(
                                (int64_t)out_pixel -
                                (int64_t)recon_coeff_buffer[k * 4 + 3 + j * recon_ptr->stride_cb]);
                        }
                    }

                    input_buffer = input_buffer_org_v + tb_origin_y * input_picture_ptr->stride_cr +
                                   tb_origin_x;
                    input_buffer_bit_inc = input_picture_ptr->buffer_bit_inc_cr +
                                           tb_origin_y * chroma_2bit_width +
                                           (tb_origin_x / 4) * sb_height;
                    recon_coeff_buffer =
                        recon_buffer_org_v + tb_origin_y * recon_ptr->stride_cr + tb_origin_x;

                    for (j = 0; j < sb_height; j++) {
                        for (k = 0; k < sb_width / 4; k++) {
                            four_2bit_pels = input_buffer_bit_inc[k + j * inn_stride];

                            n_bit_pixel = (four_2bit_pels >> 6) & 3;
                            out_pixel   = input_buffer[k * 4 + 0 + j * input_picture_ptr->stride_cr]
                                        << 2;
                            out_pixel = out_pixel | n_bit_pixel;
                            residual_distortion_v += (int64_t)SQR(
                                (int64_t)out_pixel -
                                (int64_t)recon_coeff_buffer[k * 4 + 0 + j * recon_ptr->stride_cr]);

                            n_bit_pixel = (four_2bit_pels >> 4) & 3;
                            out_pixel   = input_buffer[k * 4 + 1 + j * input_picture_ptr->stride_cr]
                                        << 2;
                            out_pixel = out_pixel | n_bit_pixel;
                            residual_distortion_v += (int64_t)SQR(
                                (int64_t)out_pixel -
                                (int64_t)recon_coeff_buffer[k * 4 + 1 + j * recon_ptr->stride_cr]);

                            n_bit_pixel = (four_2bit_pels >> 2) & 3;
                            out_pixel   = input_buffer[k * 4 + 2 + j * input_picture_ptr->stride_cr]
                                        << 2;
                            out_pixel = out_pixel | n_bit_pixel;
                            residual_distortion_v += (int64_t)SQR(
                                (int64_t)out_pixel -
                                (int64_t)recon_coeff_buffer[k * 4 + 2 + j * recon_ptr->stride_cr]);

                            n_bit_pixel = (four_2bit_pels >> 0) & 3;
                            out_pixel   = input_buffer[k * 4 + 3 + j * input_picture_ptr->stride_cr]
                                        << 2;
                            out_pixel = out_pixel | n_bit_pixel;
                            residual_distortion_v += (int64_t)SQR(
                                (int64_t)out_pixel -
                                (int64_t)recon_coeff_buffer[k * 4 + 3 + j * recon_ptr->stride_cr]);
                        }
                    }
                }
            }

            sse_total[0] = residual_distortion;
            sse_total[1] = residual_distortion_u;
            sse_total[2] = residual_distortion_v;
        } else {
            recon_coeff_buffer = (uint16_t *)(&(
                (recon_ptr->buffer_y)[(recon_ptr->origin_x << is_16bit) +
                                      (recon_ptr->origin_y << is_16bit) * recon_ptr->stride_y]));

            // if current source picture was temporally filtered, use an alternative buffer which stores
            // the original source picture
            EbByte buffer_y, buffer_bit_inc_y;
            EbByte buffer_cb, buffer_bit_inc_cb;
            EbByte buffer_cr, buffer_bit_inc_cr;

            if (pcs_ptr->parent_pcs_ptr->temporal_filtering_on == EB_TRUE) {
                buffer_y          = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_ptr[0];
                buffer_bit_inc_y  = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_bit_inc_ptr[0];
                buffer_cb         = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_ptr[1];
                buffer_bit_inc_cb = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_bit_inc_ptr[1];
                buffer_cr         = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_ptr[2];
                buffer_bit_inc_cr = pcs_ptr->parent_pcs_ptr->save_enhanced_picture_bit_inc_ptr[2];
            } else {
                buffer_y          = input_picture_ptr->buffer_y;
                buffer_bit_inc_y  = input_picture_ptr->buffer_bit_inc_y;
                buffer_cb         = input_picture_ptr->buffer_cb;
                buffer_bit_inc_cb = input_picture_ptr->buffer_bit_inc_cb;
                buffer_cr         = input_picture_ptr->buffer_cr;
                buffer_bit_inc_cr = input_picture_ptr->buffer_bit_inc_cr;
            }

            input_buffer         = &((buffer_y)[input_picture_ptr->origin_x +
                                        input_picture_ptr->origin_y * input_picture_ptr->stride_y]);
            input_buffer_bit_inc = &((buffer_bit_inc_y)[input_picture_ptr->origin_x +
                                                        input_picture_ptr->origin_y *
                                                            input_picture_ptr->stride_bit_inc_y]);

            residual_distortion = 0;

            for (int row_index = 0;
                 row_index < input_picture_ptr->height - scs_ptr->max_input_pad_bottom;
                 ++row_index) {
                for (int column_index = 0; column_index <
                     input_picture_ptr->width - scs_ptr->max_input_pad_right;
                     ++column_index) {
                    residual_distortion +=
                        (int64_t)SQR((int64_t)((((input_buffer[column_index]) << 2) |
                                                ((input_buffer_bit_inc[column_index] >> 6) & 3))) -
                                     (recon_coeff_buffer[column_index]));
                }

                input_buffer += input_picture_ptr->stride_y;
                input_buffer_bit_inc += input_picture_ptr->stride_bit_inc_y;
                recon_coeff_buffer += recon_ptr->stride_y;
            }

            sse_total[0] = residual_distortion;

            recon_coeff_buffer =
                (uint16_t *)(&((recon_ptr->buffer_cb)[(recon_ptr->origin_x << is_16bit) / 2 +
                                                      (recon_ptr->origin_y << is_16bit) / 2 *
                                                          recon_ptr->stride_cb]));
            input_buffer =
                &((buffer_cb)[input_picture_ptr->origin_x / 2 +
                              input_picture_ptr->origin_y / 2 * input_picture_ptr->stride_cb]);
            input_buffer_bit_inc = &((buffer_bit_inc_cb)[input_picture_ptr->origin_x / 2 +
                                                         input_picture_ptr->origin_y / 2 *
                                                             input_picture_ptr->stride_bit_inc_cb]);

            residual_distortion = 0;
            for (int row_index = 0; row_index <
                 (input_picture_ptr->height - scs_ptr->max_input_pad_bottom) >> ss_y;
                 ++row_index) {
                for (int column_index = 0; column_index <
                     (input_picture_ptr->width - scs_ptr->max_input_pad_right) >> ss_x;
                     ++column_index) {
                    residual_distortion +=
                        (int64_t)SQR((int64_t)((((input_buffer[column_index]) << 2) |
                                                ((input_buffer_bit_inc[column_index] >> 6) & 3))) -
                                     (recon_coeff_buffer[column_index]));
                }

                input_buffer += input_picture_ptr->stride_cb;
                input_buffer_bit_inc += input_picture_ptr->stride_bit_inc_cb;
                recon_coeff_buffer += recon_ptr->stride_cb;
            }

            sse_total[1] = residual_distortion;

            recon_coeff_buffer =
                (uint16_t *)(&((recon_ptr->buffer_cr)[(recon_ptr->origin_x << is_16bit) / 2 +
                                                      (recon_ptr->origin_y << is_16bit) / 2 *
                                                          recon_ptr->stride_cr]));
            input_buffer =
                &((buffer_cr)[input_picture_ptr->origin_x / 2 +
                              input_picture_ptr->origin_y / 2 * input_picture_ptr->stride_cr]);
            input_buffer_bit_inc = &((buffer_bit_inc_cr)[input_picture_ptr->origin_x / 2 +
                                                         input_picture_ptr->origin_y / 2 *
                                                             input_picture_ptr->stride_bit_inc_cr]);

            residual_distortion = 0;

            for (int row_index = 0; row_index <
                 (input_picture_ptr->height - scs_ptr->max_input_pad_bottom) >> ss_y;
                 ++row_index) {
                for (int column_index = 0; column_index <
                     (input_picture_ptr->width - scs_ptr->max_input_pad_right) >> ss_x;
                     ++column_index) {
                    residual_distortion +=
                        (int64_t)SQR((int64_t)((((input_buffer[column_index]) << 2) |
                                                ((input_buffer_bit_inc[column_index] >> 6) & 3))) -
                                     (recon_coeff_buffer[column_index]));
                }

                input_buffer += input_picture_ptr->stride_cr;
                input_buffer_bit_inc += input_picture_ptr->stride_bit_inc_cr;
                recon_coeff_buffer += recon_ptr->stride_cr;
            }

            sse_total[2] = residual_distortion;

            if (free_memory && pcs_ptr->parent_pcs_ptr->temporal_filtering_on == EB_TRUE) {
                EB_FREE_ARRAY(buffer_y);
                EB_FREE_ARRAY(buffer_cb);
                EB_FREE_ARRAY(buffer_cr);
                EB_FREE_ARRAY(buffer_bit_inc_y);
                EB_FREE_ARRAY(buffer_bit_inc_cb);
                EB_FREE_ARRAY(buffer_bit_inc_cr);
           }
        }

        pcs_ptr->parent_pcs_ptr->luma_sse = (uint32_t)sse_total[0];
        pcs_ptr->parent_pcs_ptr->cb_sse   = (uint32_t)sse_total[1];
        pcs_ptr->parent_pcs_ptr->cr_sse   = (uint32_t)sse_total[2];
    }
}

void pad_ref_and_set_flags(PictureControlSet *pcs_ptr, SequenceControlSet *scs_ptr) {
    EbReferenceObject *reference_object =
        (EbReferenceObject *)pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr;
    EbPictureBufferDesc *ref_pic_ptr = (EbPictureBufferDesc *)reference_object->reference_picture;
    EbPictureBufferDesc *ref_pic_16bit_ptr =
        (EbPictureBufferDesc *)reference_object->reference_picture16bit;
    EbBool is_16bit = (scs_ptr->static_config.encoder_bit_depth > EB_8BIT);

    if (!is_16bit) {
        pad_picture_to_multiple_of_min_blk_size_dimensions(scs_ptr, ref_pic_ptr);
        // Y samples
        generate_padding(ref_pic_ptr->buffer_y,
                         ref_pic_ptr->stride_y,
                         ref_pic_ptr->width,
                         ref_pic_ptr->height,
                         ref_pic_ptr->origin_x,
                         ref_pic_ptr->origin_y);

        // Cb samples
        generate_padding(ref_pic_ptr->buffer_cb,
                         ref_pic_ptr->stride_cb,
                         ref_pic_ptr->width >> 1,
                         ref_pic_ptr->height >> 1,
                         ref_pic_ptr->origin_x >> 1,
                         ref_pic_ptr->origin_y >> 1);

        // Cr samples
        generate_padding(ref_pic_ptr->buffer_cr,
                         ref_pic_ptr->stride_cr,
                         ref_pic_ptr->width >> 1,
                         ref_pic_ptr->height >> 1,
                         ref_pic_ptr->origin_x >> 1,
                         ref_pic_ptr->origin_y >> 1);
    }

    //We need this for MCP
    if (is_16bit) {
        // Non visible Reference samples should be overwritten by the last visible line of pixels
        pad_picture_to_multiple_of_min_blk_size_dimensions_16bit(scs_ptr, ref_pic_16bit_ptr);

        // Y samples
        generate_padding16_bit(ref_pic_16bit_ptr->buffer_y,
                               ref_pic_16bit_ptr->stride_y << 1,
                               ref_pic_16bit_ptr->width << 1,
                               ref_pic_16bit_ptr->height,
                               ref_pic_16bit_ptr->origin_x << 1,
                               ref_pic_16bit_ptr->origin_y);

        // Cb samples
        generate_padding16_bit(ref_pic_16bit_ptr->buffer_cb,
                               ref_pic_16bit_ptr->stride_cb << 1,
                               ref_pic_16bit_ptr->width,
                               ref_pic_16bit_ptr->height >> 1,
                               ref_pic_16bit_ptr->origin_x,
                               ref_pic_16bit_ptr->origin_y >> 1);

        // Cr samples
        generate_padding16_bit(ref_pic_16bit_ptr->buffer_cr,
                               ref_pic_16bit_ptr->stride_cr << 1,
                               ref_pic_16bit_ptr->width,
                               ref_pic_16bit_ptr->height >> 1,
                               ref_pic_16bit_ptr->origin_x,
                               ref_pic_16bit_ptr->origin_y >> 1);

        // Hsan: unpack ref samples (to be used @ MD)
        un_pack2d((uint16_t *)ref_pic_16bit_ptr->buffer_y,
                  ref_pic_16bit_ptr->stride_y,
                  ref_pic_ptr->buffer_y,
                  ref_pic_ptr->stride_y,
                  ref_pic_ptr->buffer_bit_inc_y,
                  ref_pic_ptr->stride_bit_inc_y,
                  ref_pic_16bit_ptr->width + (ref_pic_ptr->origin_x << 1),
                  ref_pic_16bit_ptr->height + (ref_pic_ptr->origin_y << 1));
        if (pcs_ptr->hbd_mode_decision != EB_10_BIT_MD) {

        un_pack2d((uint16_t *)ref_pic_16bit_ptr->buffer_cb,
                  ref_pic_16bit_ptr->stride_cb,
                  ref_pic_ptr->buffer_cb,
                  ref_pic_ptr->stride_cb,
                  ref_pic_ptr->buffer_bit_inc_cb,
                  ref_pic_ptr->stride_bit_inc_cb,
                  (ref_pic_16bit_ptr->width + (ref_pic_ptr->origin_x << 1)) >> 1,
                  (ref_pic_16bit_ptr->height + (ref_pic_ptr->origin_y << 1)) >> 1);

        un_pack2d((uint16_t *)ref_pic_16bit_ptr->buffer_cr,
                  ref_pic_16bit_ptr->stride_cr,
                  ref_pic_ptr->buffer_cr,
                  ref_pic_ptr->stride_cr,
                  ref_pic_ptr->buffer_bit_inc_cr,
                  ref_pic_ptr->stride_bit_inc_cr,
                  (ref_pic_16bit_ptr->width + (ref_pic_ptr->origin_x << 1)) >> 1,
                  (ref_pic_16bit_ptr->height + (ref_pic_ptr->origin_y << 1)) >> 1);
        }
    }
    if ((scs_ptr->static_config.is_16bit_pipeline) && (!is_16bit)) {
        // Y samples
        generate_padding16_bit(ref_pic_16bit_ptr->buffer_y,
                               ref_pic_16bit_ptr->stride_y << 1,
                               (ref_pic_16bit_ptr->width - scs_ptr->max_input_pad_right) << 1,
                               ref_pic_16bit_ptr->height - scs_ptr->max_input_pad_bottom,
                               ref_pic_16bit_ptr->origin_x << 1,
                               ref_pic_16bit_ptr->origin_y);

        // Cb samples
        generate_padding16_bit(ref_pic_16bit_ptr->buffer_cb,
                               ref_pic_16bit_ptr->stride_cb << 1,
                               (ref_pic_16bit_ptr->width - scs_ptr->max_input_pad_right),
                               (ref_pic_16bit_ptr->height - scs_ptr->max_input_pad_bottom) >> 1,
                               ref_pic_16bit_ptr->origin_x,
                               ref_pic_16bit_ptr->origin_y >> 1);

        // Cr samples
        generate_padding16_bit(ref_pic_16bit_ptr->buffer_cr,
                               ref_pic_16bit_ptr->stride_cr << 1,
                               (ref_pic_16bit_ptr->width - scs_ptr->max_input_pad_right),
                               (ref_pic_16bit_ptr->height - scs_ptr->max_input_pad_bottom) >> 1,
                               ref_pic_16bit_ptr->origin_x,
                               ref_pic_16bit_ptr->origin_y >> 1);

        // Hsan: unpack ref samples (to be used @ MD)

        //Y
        uint16_t *buf_16bit = (uint16_t *)(ref_pic_16bit_ptr->buffer_y);
        uint8_t * buf_8bit = ref_pic_ptr->buffer_y;
        svt_convert_16bit_to_8bit(buf_16bit,
            ref_pic_16bit_ptr->stride_y,
            buf_8bit,
            ref_pic_ptr->stride_y,
            ref_pic_16bit_ptr->width + (ref_pic_ptr->origin_x << 1),
            ref_pic_16bit_ptr->height + (ref_pic_ptr->origin_y << 1));

        //CB
        buf_16bit = (uint16_t *)(ref_pic_16bit_ptr->buffer_cb);
        buf_8bit = ref_pic_ptr->buffer_cb;
        svt_convert_16bit_to_8bit(buf_16bit,
            ref_pic_16bit_ptr->stride_cb,
            buf_8bit,
            ref_pic_ptr->stride_cb,
            (ref_pic_16bit_ptr->width + (ref_pic_ptr->origin_x << 1)) >> 1,
            (ref_pic_16bit_ptr->height + (ref_pic_ptr->origin_y << 1)) >> 1);

        //CR
        buf_16bit = (uint16_t *)(ref_pic_16bit_ptr->buffer_cr);
        buf_8bit = ref_pic_ptr->buffer_cr;
        svt_convert_16bit_to_8bit(buf_16bit,
            ref_pic_16bit_ptr->stride_cr,
            buf_8bit,
            ref_pic_ptr->stride_cr,
            (ref_pic_16bit_ptr->width + (ref_pic_ptr->origin_x << 1)) >> 1,
            (ref_pic_16bit_ptr->height + (ref_pic_ptr->origin_y << 1)) >> 1);
    }
    // Save down scaled reference for HME
    if (scs_ptr->in_loop_me) {
        if (scs_ptr->down_sampling_method_me_search == ME_FILTERED_DOWNSAMPLED) {
            downsample_filtering_input_picture(
                    pcs_ptr->parent_pcs_ptr,
                    ref_pic_ptr,
                    reference_object->quarter_reference_picture,
                    reference_object->sixteenth_reference_picture);
        } else {
            downsample_decimation_input_picture(
                    pcs_ptr->parent_pcs_ptr,
                    ref_pic_ptr,
                    reference_object->quarter_reference_picture,
                    reference_object->sixteenth_reference_picture);
        }
    }
    // set up the ref POC
    reference_object->ref_poc = pcs_ptr->parent_pcs_ptr->picture_number;

    // set up the QP
    reference_object->qp = (uint8_t)pcs_ptr->parent_pcs_ptr->picture_qp;

    // set up the Slice Type
    reference_object->slice_type          = pcs_ptr->parent_pcs_ptr->slice_type;
    reference_object->r0 = pcs_ptr->parent_pcs_ptr->r0;
}

void copy_statistics_to_ref_obj_ect(PictureControlSet *pcs_ptr, SequenceControlSet *scs_ptr) {
#if !TUNE_REMOVE_INTRA_STATS_TRACKING
    pcs_ptr->intra_coded_area =
        (100 * pcs_ptr->intra_coded_area) /
        (pcs_ptr->parent_pcs_ptr->aligned_width * pcs_ptr->parent_pcs_ptr->aligned_height);
#endif
#if !CLN_REMOVE_UNUSED_CODE
    memcpy(((EbReferenceObject *)pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
                    ->ref_part_cnt, pcs_ptr->part_cnt, sizeof(uint32_t) * (NUMBER_OF_SHAPES-1) * FB_NUM *SSEG_NUM);
#endif
#if !CLN_NSQ_AND_STATS
    memcpy(((EbReferenceObject *)pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
        ->ref_pred_depth_count, pcs_ptr->pred_depth_count, sizeof(uint32_t) * DEPTH_DELTA_NUM * (NUMBER_OF_SHAPES-1));
#endif
#if !TUNE_REMOVE_TXT_STATS
    memcpy(((EbReferenceObject *)pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
            ->ref_txt_cnt, pcs_ptr->txt_cnt, sizeof(uint32_t) * TXT_DEPTH_DELTA_NUM *TX_TYPES);
#endif
#if !TUNE_REMOVE_INTRA_STATS_TRACKING
    if (pcs_ptr->slice_type == I_SLICE) pcs_ptr->intra_coded_area = 0;
#endif
#if !TUNE_REMOVE_INTRA_STATS_TRACKING
    ((EbReferenceObject *)pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
        ->intra_coded_area = (uint8_t)(pcs_ptr->intra_coded_area);
#endif
    uint32_t sb_index;
    for (sb_index = 0; sb_index < pcs_ptr->sb_total_count; ++sb_index)
        ((EbReferenceObject *)pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
            ->non_moving_index_array[sb_index] =
            pcs_ptr->parent_pcs_ptr->non_moving_index_array[sb_index];
    ((EbReferenceObject *)pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
        ->tmp_layer_idx = (uint8_t)pcs_ptr->temporal_layer_index;
    ((EbReferenceObject *)pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
        ->is_scene_change = pcs_ptr->parent_pcs_ptr->scene_change_flag;

    Av1Common *cm = pcs_ptr->parent_pcs_ptr->av1_cm;
    ((EbReferenceObject *)pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
        ->sg_frame_ep = cm->sg_frame_ep;
    if (scs_ptr->mfmv_enabled) {
        ((EbReferenceObject *)pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
            ->frame_type = pcs_ptr->parent_pcs_ptr->frm_hdr.frame_type;
        ((EbReferenceObject *)pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
            ->order_hint = pcs_ptr->parent_pcs_ptr->cur_order_hint;
        svt_memcpy(((EbReferenceObject *)
                   pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
                   ->ref_order_hint,
                   pcs_ptr->parent_pcs_ptr->ref_order_hint,
                   7 * sizeof(uint32_t));
    }
}

#if FTR_NEW_REF_PRUNING_CTRLS
void set_obmc_controls(ModeDecisionContext *mdctxt, uint8_t obmc_mode) {
    ObmcControls*obmc_ctrls = &mdctxt->obmc_ctrls;
    switch (obmc_mode)
    {
    case 0:
        obmc_ctrls->enabled = 0;
        break;
    case 1:
        obmc_ctrls->enabled = 1;
        obmc_ctrls->max_blk_size_16x16 = 0;
        break;
    case 2:
        obmc_ctrls->enabled = 1;
        obmc_ctrls->max_blk_size_16x16 = 1;
        break;
    default:
        assert(0);
        break;
    }
}
#else
void set_obmc_controls(ModeDecisionContext *mdctxt, uint8_t obmc_mode) {

    ObmcControls*obmc_ctrls = &mdctxt->obmc_ctrls;

    switch (obmc_mode)
    {
    case 0:
        obmc_ctrls->enabled = 0;
        obmc_ctrls->pme_best_ref = 0;
        obmc_ctrls->me_count = 0;
        obmc_ctrls->mvp_ref_count = 0;
        obmc_ctrls->near_count = 0;
        obmc_ctrls->max_blk_size_16x16 = 0;
        break;
    case 1:
        obmc_ctrls->enabled = 1;
        obmc_ctrls->pme_best_ref = 0;
        obmc_ctrls->me_count = ~0;
        obmc_ctrls->mvp_ref_count = 4;
        obmc_ctrls->near_count = 3;
        obmc_ctrls->max_blk_size_16x16 = 0;
        break;
    case 2:
        obmc_ctrls->enabled = 1;
        obmc_ctrls->pme_best_ref = 1;
        obmc_ctrls->me_count = ~0;
        obmc_ctrls->mvp_ref_count = 4;
        obmc_ctrls->near_count = 3;
        obmc_ctrls->max_blk_size_16x16 = 0;
        break;
    case 3:
        obmc_ctrls->enabled = 1;
        obmc_ctrls->pme_best_ref = 1;
        obmc_ctrls->me_count = ~0;
        obmc_ctrls->mvp_ref_count = 1;
        obmc_ctrls->near_count = 3;
        obmc_ctrls->max_blk_size_16x16 = 1;
        break;
    default:
        assert(0);
        break;
    }
}
#endif
#if TUNE_IMPROVE_DEPTH_REFINEMENT
void set_block_based_depth_refinement_controls(ModeDecisionContext* mdctxt, uint8_t block_based_depth_refinement_level) {

    DepthRefinementCtrls* depth_refinement_ctrls = &mdctxt->depth_refinement_ctrls;

    switch (block_based_depth_refinement_level)
    {
    case 0:
        depth_refinement_ctrls->enabled = 0;
        break;

    case 1:
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = 25;
        depth_refinement_ctrls->sub_to_current_th = 25;
        depth_refinement_ctrls->use_pred_block_cost = 0;
#if FTR_IMPROVE_DEPTH_REFINEMENT
        depth_refinement_ctrls->up_to_2_depth              =   0;
#endif
        break;

#if TUNE_NEW_PRESETS_MR_M8
    case 2:
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = 25;
        depth_refinement_ctrls->sub_to_current_th = 25;
        depth_refinement_ctrls->use_pred_block_cost = 1;
#if FTR_IMPROVE_DEPTH_REFINEMENT
        depth_refinement_ctrls->up_to_2_depth              =   0;
#endif
        break;
#endif

#if TUNE_NEW_PRESETS_MR_M8
    case 3:
#else
    case 2:
#endif
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = 20;
        depth_refinement_ctrls->sub_to_current_th = 20;
        depth_refinement_ctrls->use_pred_block_cost = 1;
#if FTR_IMPROVE_DEPTH_REFINEMENT
        depth_refinement_ctrls->up_to_2_depth              =   0;
#endif
        break;
#if TUNE_DEPTH_SKIP
    case 4:
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = 15;
        depth_refinement_ctrls->sub_to_current_th = 15;
        depth_refinement_ctrls->use_pred_block_cost = 1;
#if FTR_IMPROVE_DEPTH_REFINEMENT
        depth_refinement_ctrls->up_to_2_depth              =   0;
#endif
        break;
    case 5:
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = 10;
        depth_refinement_ctrls->sub_to_current_th = 10;
        depth_refinement_ctrls->use_pred_block_cost = 1;
#if FTR_IMPROVE_DEPTH_REFINEMENT
        depth_refinement_ctrls->up_to_2_depth              =   0;
#endif
        break;
    case 6:
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = 10;
        depth_refinement_ctrls->sub_to_current_th = 10;
        depth_refinement_ctrls->use_pred_block_cost = 2;
#if FTR_IMPROVE_DEPTH_REFINEMENT
        depth_refinement_ctrls->up_to_2_depth              =   0;
#endif
        break;
    case 7:
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = 5;
        depth_refinement_ctrls->sub_to_current_th = 5;
        depth_refinement_ctrls->use_pred_block_cost = 2;
#if FTR_IMPROVE_DEPTH_REFINEMENT
        depth_refinement_ctrls->up_to_2_depth              =   0;
#endif
        break;
#else
#if TUNE_NEW_PRESETS_MR_M8
    case 4:
#else
    case 3:
#endif
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = 5;
        depth_refinement_ctrls->sub_to_current_th = 20;
        depth_refinement_ctrls->use_pred_block_cost = 1;
        break;

#if TUNE_NEW_PRESETS_MR_M8
    case 5:
#else
    case 4:
#endif
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = 5;
        depth_refinement_ctrls->sub_to_current_th = 5;
        depth_refinement_ctrls->use_pred_block_cost = 1;
        break;

#if TUNE_NEW_PRESETS_MR_M8
    case 6:
#else
    case 5:
#endif
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = 5;
        depth_refinement_ctrls->sub_to_current_th = 5;
        depth_refinement_ctrls->use_pred_block_cost = 2;
        break;
    default:
        assert(0);
        break;
#if TUNE_M9_LEVELS
    case 7:
        depth_refinement_ctrls->enabled = 1;
#if TUNE_M7_M9
        depth_refinement_ctrls->parent_to_current_th = 5;
        depth_refinement_ctrls->sub_to_current_th = 5;
        depth_refinement_ctrls->use_pred_block_cost = 3;
#else
        depth_refinement_ctrls->parent_to_current_th = -5;
        depth_refinement_ctrls->sub_to_current_th = -5;
        depth_refinement_ctrls->use_pred_block_cost = 2;
#endif
        break;

    case 8:
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = -15;
        depth_refinement_ctrls->sub_to_current_th = -15;
#if TUNE_M7_M9
        depth_refinement_ctrls->use_pred_block_cost = 4;
#else
        depth_refinement_ctrls->use_pred_block_cost = 2;
#endif
        break;
#endif
#endif
    }
}
#else
#if FTR_EARLY_DEPTH_REMOVAL
void set_block_based_depth_refinement_controls(ModeDecisionContext *mdctxt, uint8_t block_based_depth_refinement_level) {
#else
void set_block_based_depth_refinement_controls(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr, ModeDecisionContext *mdctxt, uint8_t block_based_depth_refinement_level, uint32_t sb_width, uint32_t sb_height) {
#endif
    DepthRefinementCtrls *depth_refinement_ctrls = &mdctxt->depth_refinement_ctrls;
    switch (block_based_depth_refinement_level)
    {
    case 0:
        depth_refinement_ctrls->enabled = 0;
        break;
    case 1:
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = 25;
        depth_refinement_ctrls->sub_to_current_th = 25;
        depth_refinement_ctrls->use_pred_block_cost = 0;
#if !FTR_EARLY_DEPTH_REMOVAL
        depth_refinement_ctrls->disallow_below_16x16 = 0;
#endif
#if FTR_PD2_BLOCK_REDUCTION
        depth_refinement_ctrls->use_sb_class = 0;
#endif
        break;
    case 2:
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = 5;
        depth_refinement_ctrls->sub_to_current_th = 20;
        depth_refinement_ctrls->use_pred_block_cost = 1;
#if !FTR_EARLY_DEPTH_REMOVAL
        depth_refinement_ctrls->disallow_below_16x16 = 0;
#endif
#if FTR_PD2_BLOCK_REDUCTION
        depth_refinement_ctrls->use_sb_class = 0;
#endif
        break;
    case 3:
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = 0;
        depth_refinement_ctrls->sub_to_current_th = 15;
        depth_refinement_ctrls->use_pred_block_cost = 1;
#if !FTR_EARLY_DEPTH_REMOVAL
        depth_refinement_ctrls->disallow_below_16x16 = 0;
#endif
#if FTR_PD2_BLOCK_REDUCTION
        depth_refinement_ctrls->use_sb_class = 0;
#endif
        break;
    case 4:
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = -5;
        depth_refinement_ctrls->sub_to_current_th = 10;
        depth_refinement_ctrls->use_pred_block_cost = 1;
#if !FTR_EARLY_DEPTH_REMOVAL
        depth_refinement_ctrls->disallow_below_16x16 = 0;
#endif
#if FTR_PD2_BLOCK_REDUCTION
        depth_refinement_ctrls->use_sb_class = 0;
#endif
        break;
    case 5:
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = -10;
        depth_refinement_ctrls->sub_to_current_th = 5;
        depth_refinement_ctrls->use_pred_block_cost = 1;
#if !FTR_EARLY_DEPTH_REMOVAL
        depth_refinement_ctrls->disallow_below_16x16 =
            (pcs_ptr->slice_type != I_SLICE && scs_ptr->static_config.super_block_size == 64 && sb_width % 16 == 0 && sb_height % 16 == 0)
                ? (pcs_ptr->parent_pcs_ptr->rc_me_distortion[mdctxt->sb_index] < ((5 * 64 * 64) / 4)) : 0;
#endif
#if FTR_PD2_BLOCK_REDUCTION
        depth_refinement_ctrls->use_sb_class = 0;
#endif
        break;
#if FTR_PD2_BLOCK_REDUCTION
    case 6:
        depth_refinement_ctrls->enabled = 1;
        depth_refinement_ctrls->parent_to_current_th = -10;
        depth_refinement_ctrls->sub_to_current_th = 5;
        depth_refinement_ctrls->use_pred_block_cost = 1;
#if !FTR_EARLY_DEPTH_REMOVAL
        depth_refinement_ctrls->disallow_below_16x16 =
            (pcs_ptr->slice_type != I_SLICE && scs_ptr->static_config.super_block_size == 64 && sb_width % 16 == 0 && sb_height % 16 == 0)
                ? (pcs_ptr->parent_pcs_ptr->rc_me_distortion[mdctxt->sb_index] < ((5 * 64 * 64) / 4)) : 0;
#endif
#if FTR_PD2_BLOCK_REDUCTION
        depth_refinement_ctrls->use_sb_class = 1;
#endif
        break;
#endif
    default:
        assert(0);
        break;
    }
}
#endif
#if FTR_EARLY_DEPTH_REMOVAL
/*
 * Generate depth removal settings
 */
#if TUNE_DEPTH_REMOVAL_PER_RESOLUTION
void set_depth_removal_level_controls(PictureControlSet *pcs_ptr, ModeDecisionContext *mdctxt, uint8_t block_based_depth_refinement_level) {
    DepthRemovalCtrls *depth_removal_ctrls = &mdctxt->depth_removal_ctrls;
#if !FIX_DEPTH_REMOVAL_MODULATION
    const double noise_level = pcs_ptr->parent_pcs_ptr->noise_levels[0];
#endif
    const uint32_t me_8x8_cost_variance = pcs_ptr->parent_pcs_ptr->me_8x8_cost_variance[mdctxt->sb_index];
    SbParams *sb_params = &pcs_ptr->parent_pcs_ptr->sb_params_array[mdctxt->sb_index];
    uint32_t fast_lambda = mdctxt->hbd_mode_decision ?
        mdctxt->fast_lambda_md[EB_10_BIT_MD] :
        mdctxt->fast_lambda_md[EB_8_BIT_MD];
    uint32_t sb_size = 64 * 64;
    uint64_t cost_th_rate = 1 << 13;
    uint64_t disallow_below_64x64_th = 0;
    uint64_t disallow_below_32x32_th = 0;
    uint64_t disallow_below_16x16_th = 0;
    int64_t dev_16x16_to_8x8_th = MAX_SIGNED_VALUE;
#if TUNE_M9_DEPTH_REMOVAL
    int64_t dev_32x32_to_16x16_th = 0;
#endif

    switch (block_based_depth_refinement_level) {
    case 0:
        depth_removal_ctrls->enabled = 0;
        break;
    case 1:
        depth_removal_ctrls->enabled = 1;
        if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 200) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
        }
        else if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 400) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);
        }
        else {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = 0;
        }

        dev_16x16_to_8x8_th = 2;
#if !FIX_DEPTH_REMOVAL_MODULATION
        if (noise_level < 0.1 )
            dev_16x16_to_8x8_th *=  (dev_16x16_to_8x8_th * 15)/10;
        else if(noise_level < 0.5 )
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 12)/10;
#endif
        if(me_8x8_cost_variance < 2000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
        else if(me_8x8_cost_variance <7000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 20) / 5;
        else if(me_8x8_cost_variance <15000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 10) / 5;
        else if(me_8x8_cost_variance <30000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 2) / 5;
        else
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 1) / 5;

        break;
    case 2:
        depth_removal_ctrls->enabled = 1;

        if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 200) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
        }
        else if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 400) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);
        }
        else {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = 0;
        }

        dev_16x16_to_8x8_th = 2;
#if !FIX_DEPTH_REMOVAL_MODULATION
        if (noise_level < 0.1 )
            dev_16x16_to_8x8_th *=  (dev_16x16_to_8x8_th * 15)/10;
        else if(noise_level < 0.5 )
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 12)/10;
#endif
        if(me_8x8_cost_variance < 2000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
        else if(me_8x8_cost_variance <7000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 20) / 5;
        else if(me_8x8_cost_variance <15000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 10) / 5;
        else if(me_8x8_cost_variance <30000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 3) / 5;
        else
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 2) / 5;

        break;
    case 3:

        depth_removal_ctrls->enabled = 1;

        if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 200) {
            disallow_below_64x64_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);
            disallow_below_32x32_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 16);
        }
        else if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 400) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 12);
        }
        else {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 2);
        }

        dev_16x16_to_8x8_th = 2;
#if !FIX_DEPTH_REMOVAL_MODULATION
        if (noise_level < 0.1 )
            dev_16x16_to_8x8_th *=  (dev_16x16_to_8x8_th * 15)/10;
        else if(noise_level < 0.5 )
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 12)/10;
#endif
        if(me_8x8_cost_variance <2000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
        else if(me_8x8_cost_variance <8000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 20) / 5;
        else if(me_8x8_cost_variance <13000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 10) / 5;
        else if(me_8x8_cost_variance <25000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 3) / 5;
        else
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 2) / 5;
        break;
    case 4:
        depth_removal_ctrls->enabled = 1;

        if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 200) {
            disallow_below_64x64_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);
            disallow_below_32x32_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 16);
        }
        else if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 400) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 12);
        }
        else {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 2);

        }

        dev_16x16_to_8x8_th = 2;
#if !FIX_DEPTH_REMOVAL_MODULATION
        if (noise_level < 0.1 )
            dev_16x16_to_8x8_th *=  (dev_16x16_to_8x8_th * 17)/10;
        else if(noise_level < 0.5 )
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 13)/10;
#endif
        if(me_8x8_cost_variance <5000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
        else if(me_8x8_cost_variance <10000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 20) / 5;
        else if(me_8x8_cost_variance <20000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 10) / 5;
        else if(me_8x8_cost_variance <40000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 3) / 5;
        else
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 2) / 5;
        break;

    case 5:
        depth_removal_ctrls->enabled = 1;

        if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 200) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
        }
        else if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 400) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);
        }
        else {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = 0;
        }

        dev_16x16_to_8x8_th = 2;
#if !FIX_DEPTH_REMOVAL_MODULATION
        if (noise_level < 0.1 )
            dev_16x16_to_8x8_th *=  (dev_16x16_to_8x8_th * 15)/10;
        else if(noise_level < 0.5 )
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 12)/10;
#endif
        if(me_8x8_cost_variance < 3500)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
        else if(me_8x8_cost_variance <13000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 20) / 5;
        else if(me_8x8_cost_variance <30000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 10) / 5;
        else if(me_8x8_cost_variance <50000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 2) / 5;
        else
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 1) / 5;
        break;
    case 6:
        depth_removal_ctrls->enabled = 1;
        if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 200) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
        }
        else if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 400) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);
        }
        else {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = 0;
        }
        dev_16x16_to_8x8_th = 2;
#if TUNE_M9_DEPTH_REMOVAL
#if FIX_DEPTH_REMOVAL_SETTINGS
        dev_32x32_to_16x16_th = 2;
#else
        dev_32x32_to_16x16_th = 1;
#endif
#endif
#if !FIX_DEPTH_REMOVAL_MODULATION
        if (noise_level < 0.1 )
            dev_16x16_to_8x8_th *=  (dev_16x16_to_8x8_th * 15)/10;
        else if(noise_level < 0.5 )
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 12)/10;
#endif
#if TUNE_M9_DEPTH_REMOVAL
        if (me_8x8_cost_variance < 9000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 20) / 5;
        }
        else if (me_8x8_cost_variance < 20000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 20) / 5;
        }
        else if (me_8x8_cost_variance < 50000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 10) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 4) / 5;
        }
        else if (me_8x8_cost_variance < 70000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 3) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 3) / 5;
        }
        else {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 2) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 2) / 5;
        }
#else
        if(me_8x8_cost_variance < 9000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
        else if(me_8x8_cost_variance <20000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 20) / 5;
        else if(me_8x8_cost_variance <50000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 10) / 5;
        else if(me_8x8_cost_variance <70000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 3) / 5;
        else
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 2) / 5;
#endif
        break;
    case 7:
        depth_removal_ctrls->enabled = 1;

        if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 200) {
            disallow_below_64x64_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);
            disallow_below_32x32_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 16);
        }
        else if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 400) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 12);
        }
        else {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 2);
        }

        dev_16x16_to_8x8_th = 2;
#if TUNE_M9_DEPTH_REMOVAL
#if FIX_DEPTH_REMOVAL_SETTINGS
        dev_32x32_to_16x16_th = 2;
#else
        dev_32x32_to_16x16_th = 1;
#endif
#endif
#if !FIX_DEPTH_REMOVAL_MODULATION
        if (noise_level < 0.1 )
            dev_16x16_to_8x8_th *=  (dev_16x16_to_8x8_th * 15)/10;
        else if(noise_level < 0.5 )
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 12)/10;
#endif
#if TUNE_M9_DEPTH_REMOVAL
        if (me_8x8_cost_variance < 9000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 20) / 5;
        }
        else if (me_8x8_cost_variance < 20000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 20) / 5;
        }
        else if (me_8x8_cost_variance < 50000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 10) / 5;
        }
        else if (me_8x8_cost_variance < 70000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 3) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 3) / 5;
        }
        else {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 2) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 2) / 5;
        }
#else
        if(me_8x8_cost_variance <4500)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
        else if(me_8x8_cost_variance <12000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 20) / 5;
        else if(me_8x8_cost_variance <25000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 10) / 5;
        else if(me_8x8_cost_variance <40000)
        dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 3) / 5;
        else
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 2) / 5;
#endif
        break;
    case 8:
        depth_removal_ctrls->enabled = 1;

        if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 200) {
            disallow_below_64x64_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);
            disallow_below_32x32_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 16);
        }
        else if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 400) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 12);
        }
        else {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 2);

        }

        dev_16x16_to_8x8_th = 2;
#if TUNE_M9_DEPTH_REMOVAL
#if FIX_DEPTH_REMOVAL_SETTINGS
        dev_32x32_to_16x16_th = 2;
#else
        dev_32x32_to_16x16_th = 1;
#endif
#endif
#if !FIX_DEPTH_REMOVAL_MODULATION
        if (noise_level < 0.1 )
            dev_16x16_to_8x8_th *=  (dev_16x16_to_8x8_th * 17)/10;
        else if(noise_level < 0.5 )
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 13)/10;
#endif
#if TUNE_M9_DEPTH_REMOVAL
        if (me_8x8_cost_variance < 50000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 20) / 5;
        }
        else if (me_8x8_cost_variance < 100000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 20) / 5;
        }
        else if (me_8x8_cost_variance < 150000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 10) / 5;
        }
        else if (me_8x8_cost_variance < 200000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 3) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 3) / 5;
        }
        else {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 2) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 2) / 5;
        }
#else
        if(me_8x8_cost_variance <50000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
        else if(me_8x8_cost_variance <100000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 20) / 5;
        else if(me_8x8_cost_variance <150000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 10) / 5;
        else if(me_8x8_cost_variance <200000)
        dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 3) / 5;
        else
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 2) / 5;
#endif
        break;
#if TUNE_M9_OPT_DEPTH_REMOVAL
    case 9:
        depth_removal_ctrls->enabled = 1;

        disallow_below_64x64_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
        disallow_below_32x32_th = RDCOST(fast_lambda, cost_th_rate, (sb_size * 3) >> 1);
        disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 32);

#if FIX_DEPTH_REMOVAL_MODULATION
        dev_16x16_to_8x8_th = 2;
#if FIX_DEPTH_REMOVAL_SETTINGS
        dev_32x32_to_16x16_th = 2;
#else
        dev_32x32_to_16x16_th = 1;
#endif
#else
        dev_16x16_to_8x8_th = 4;
#if TUNE_M9_OPT_DEPTH_REMOVAL
        dev_32x32_to_16x16_th = 2;
#endif
#endif
#if !FIX_DEPTH_REMOVAL_MODULATION
        if (noise_level < 0.1 )
            dev_16x16_to_8x8_th *=  (dev_16x16_to_8x8_th * 17)/10;
        else if(noise_level < 0.5 )
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 13)/10;
#endif
#if TUNE_M9_DEPTH_REMOVAL
        if (me_8x8_cost_variance < 50000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 10) / 5;
        }
        else if (me_8x8_cost_variance < 100000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 20) / 5;
        }
        else if (me_8x8_cost_variance < 150000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 10) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 2) / 5;
        }
        else if (me_8x8_cost_variance < 200000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 3) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 2) / 5;
        }
        else {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 2) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 2) / 5;
        }
#else
        if(me_8x8_cost_variance <50000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
        else if(me_8x8_cost_variance <100000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 20) / 5;
        else if(me_8x8_cost_variance <150000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 10) / 5;
        else if(me_8x8_cost_variance <200000)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 3) / 5;
        else
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 2) / 5;
#endif
        break;
#if TUNE_DEPTH_REMOVAL_M9
    case 10:
        depth_removal_ctrls->enabled = 1;
        disallow_below_64x64_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
        disallow_below_32x32_th = RDCOST(fast_lambda, cost_th_rate, (sb_size * 3) >> 1);
        disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 32);
#if FIX_DEPTH_REMOVAL_SETTINGS
        dev_16x16_to_8x8_th = 4;
        dev_32x32_to_16x16_th = 3;
#else
#if FIX_DEPTH_REMOVAL_MODULATION
        dev_16x16_to_8x8_th = 4;
        dev_32x32_to_16x16_th = 2;
#else
        dev_16x16_to_8x8_th = 2;
        dev_32x32_to_16x16_th = 1;
#endif
#endif
#if !FIX_DEPTH_REMOVAL_MODULATION
        if (noise_level < 0.1)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 17) / 10;
        else if (noise_level < 0.5)
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 13) / 10;
#endif
        if (me_8x8_cost_variance < 50000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 10) / 5;
        }
        else if (me_8x8_cost_variance < 100000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 20) / 5;
        }
        else if (me_8x8_cost_variance < 150000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 10) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 2) / 5;
        }
        else if (me_8x8_cost_variance < 200000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 3) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 2) / 5;
        }
        else {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 2) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 2) / 5;
        }
        break;
#if FIX_DEPTH_REMOVAL_SETTINGS
    case 11:
        depth_removal_ctrls->enabled = 1;

        disallow_below_64x64_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);
        disallow_below_32x32_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);
        disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 32);

        dev_16x16_to_8x8_th = 4;
        dev_32x32_to_16x16_th = 4;

        if (me_8x8_cost_variance < 50000) {
            dev_16x16_to_8x8_th *= (dev_16x16_to_8x8_th * 50) / 5;
            dev_32x32_to_16x16_th *= (dev_32x32_to_16x16_th * 10) / 5;
        }
        else if (me_8x8_cost_variance < 200000) {
            dev_16x16_to_8x8_th *= dev_16x16_to_8x8_th * 4;
            dev_32x32_to_16x16_th *= dev_32x32_to_16x16_th * 2;
        }
        else {
            dev_16x16_to_8x8_th = dev_16x16_to_8x8_th * 2;
        }
        break;
#endif
#endif
    }

    depth_removal_ctrls->disallow_below_64x64 = 0;
    depth_removal_ctrls->disallow_below_32x32 = 0;
    depth_removal_ctrls->disallow_below_16x16 = 0;

    if (depth_removal_ctrls->enabled) {

        uint64_t cost_64x64 = RDCOST(fast_lambda, 0, pcs_ptr->parent_pcs_ptr->me_64x64_distortion[mdctxt->sb_index]);
        uint64_t cost_32x32 = RDCOST(fast_lambda, 0, pcs_ptr->parent_pcs_ptr->me_32x32_distortion[mdctxt->sb_index]);
        uint64_t cost_16x16 = RDCOST(fast_lambda, 0, pcs_ptr->parent_pcs_ptr->me_16x16_distortion[mdctxt->sb_index]);
        uint64_t cost_8x8 = RDCOST(fast_lambda, 0, pcs_ptr->parent_pcs_ptr->me_8x8_distortion[mdctxt->sb_index]);
#if TUNE_M9_DEPTH_REMOVAL
        int64_t dev_32x32_to_16x16 =
            (int64_t)(((int64_t)MAX(cost_32x32, 1) - (int64_t)MAX(cost_16x16, 1)) * 100) /
            (int64_t)MAX(cost_16x16, 1);
#endif
#endif
#if FIX_DEPTH_REMOVAL_MODULATION
        int64_t dev_32x32_to_8x8 =
            (int64_t)(((int64_t)MAX(cost_32x32, 1) - (int64_t)MAX(cost_8x8, 1)) * 100) /
            (int64_t)MAX(cost_8x8, 1);
#if FIX_DEPTH_REMOVAL_SETTINGS
        int64_t dev_32x32_to_8x8_th = (dev_32x32_to_16x16_th * 5) / 4;
#else
        int64_t dev_32x32_to_8x8_th = dev_32x32_to_16x16_th;
#endif
#endif
        int64_t dev_16x16_to_8x8 =
            (int64_t)(((int64_t)MAX(cost_16x16, 1) - (int64_t)MAX(cost_8x8, 1)) * 100) /
            (int64_t)MAX(cost_8x8, 1);

        depth_removal_ctrls->disallow_below_64x64 = (sb_params->width % 64 == 0 && sb_params->height % 64 == 0)
            ? (cost_64x64 < disallow_below_64x64_th)
            : 0;

        depth_removal_ctrls->disallow_below_32x32 = (sb_params->width % 32 == 0 && sb_params->height % 32 == 0)
#if TUNE_M9_DEPTH_REMOVAL
#if FIX_DEPTH_REMOVAL_MODULATION
            ? (cost_32x32 < disallow_below_32x32_th || (dev_32x32_to_16x16 < dev_32x32_to_16x16_th && dev_32x32_to_8x8 < dev_32x32_to_8x8_th))
#else
            ? (cost_32x32 < disallow_below_32x32_th || dev_32x32_to_16x16 < dev_32x32_to_16x16_th)
#endif
#else
            ? (cost_32x32 < disallow_below_32x32_th)
#endif
            : 0;

        depth_removal_ctrls->disallow_below_16x16 = (sb_params->width % 16 == 0 && sb_params->height % 16 == 0)
            ? (cost_16x16 < disallow_below_16x16_th || dev_16x16_to_8x8 < dev_16x16_to_8x8_th)
            : 0;
    }
}
#else
void set_depth_removal_level_controls(PictureControlSet *pcs_ptr, ModeDecisionContext *mdctxt, uint8_t block_based_depth_refinement_level) {
    DepthRemovalCtrls *depth_removal_ctrls = &mdctxt->depth_removal_ctrls;

    SbParams *sb_params = &pcs_ptr->parent_pcs_ptr->sb_params_array[mdctxt->sb_index];

    uint32_t fast_lambda = mdctxt->hbd_mode_decision ?
        mdctxt->fast_lambda_md[EB_10_BIT_MD] :
        mdctxt->fast_lambda_md[EB_8_BIT_MD];

    uint32_t sb_size = 64 * 64;
    uint64_t cost_th_rate = 1 << 13;

    uint64_t disallow_below_64x64_th = 0;
    uint64_t disallow_below_32x32_th = 0;
    uint64_t disallow_below_16x16_th = 0;
    int64_t dev_16x16_to_8x8_th = MAX_SIGNED_VALUE;

    switch (block_based_depth_refinement_level)
    {
    case 0:
        depth_removal_ctrls->enabled = 0;
        break;

    case 1:
        depth_removal_ctrls->enabled = 1;
#if TUNE_M4_M8
        dev_16x16_to_8x8_th = 5;

        if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 200) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
        }
        else if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 400) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);
        }
        else {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = 0;
        }
#else
        dev_16x16_to_8x8_th = 10;
        if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 200) {
            disallow_below_64x64_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);
            disallow_below_32x32_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 2);
        }
        else if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 400) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
        }
        else {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);
        }
#endif
        break;

    case 2:
        depth_removal_ctrls->enabled = 1;
        dev_16x16_to_8x8_th = 10;
        if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 200) {
            disallow_below_64x64_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);
            disallow_below_32x32_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 16);
        }
        else if (pcs_ptr->parent_pcs_ptr->variance[mdctxt->sb_index][ME_TIER_ZERO_PU_64x64] <= 400) {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 12);
        }
        else {
            disallow_below_64x64_th = 0;
            disallow_below_32x32_th = 0;
            disallow_below_16x16_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 2);
        }
        break;

    }
    depth_removal_ctrls->disallow_below_64x64 = 0;
    depth_removal_ctrls->disallow_below_32x32 = 0;
    depth_removal_ctrls->disallow_below_16x16 = 0;

    if (depth_removal_ctrls->enabled) {

        uint64_t cost_64x64 = RDCOST(fast_lambda, 0, pcs_ptr->parent_pcs_ptr->me_64x64_distortion[mdctxt->sb_index]);
        uint64_t cost_32x32 = RDCOST(fast_lambda, 0, pcs_ptr->parent_pcs_ptr->me_32x32_distortion[mdctxt->sb_index]);
        uint64_t cost_16x16 = RDCOST(fast_lambda, 0, pcs_ptr->parent_pcs_ptr->me_16x16_distortion[mdctxt->sb_index]);
        uint64_t cost_8x8 = RDCOST(fast_lambda, 0, pcs_ptr->parent_pcs_ptr->me_8x8_distortion[mdctxt->sb_index]);

        int64_t dev_16x16_to_8x8 =
            (int64_t)(((int64_t)MAX(cost_16x16, 1) - (int64_t)MAX(cost_8x8, 1)) * 100) /
            (int64_t)MAX(cost_8x8, 1);

        depth_removal_ctrls->disallow_below_64x64 = (sb_params->width % 64 == 0 && sb_params->height % 64 == 0)
            ? (cost_64x64 < disallow_below_64x64_th)
            : 0;

        depth_removal_ctrls->disallow_below_32x32 = (sb_params->width % 32 == 0 && sb_params->height % 32 == 0)
            ? (cost_32x32 < disallow_below_32x32_th)
            : 0;

        depth_removal_ctrls->disallow_below_16x16 = (sb_params->width % 16 == 0 && sb_params->height % 16 == 0)
            ? (cost_16x16 < disallow_below_16x16_th || dev_16x16_to_8x8 < dev_16x16_to_8x8_th)
            : 0;
    }
}
#endif
#endif
/*
 * Control NSQ search
 */
void md_nsq_motion_search_controls(ModeDecisionContext *mdctxt, uint8_t md_nsq_mv_search_level) {

    MdNsqMotionSearchCtrls *md_nsq_motion_search_ctrls = &mdctxt->md_nsq_motion_search_ctrls;

    switch (md_nsq_mv_search_level)
    {
    case 0:
        md_nsq_motion_search_ctrls->enabled = 0;
        break;
    case 1:
        md_nsq_motion_search_ctrls->enabled = 1;
        md_nsq_motion_search_ctrls->use_ssd = 0;
        md_nsq_motion_search_ctrls->full_pel_search_width = 31;
        md_nsq_motion_search_ctrls->full_pel_search_height = 31;
        break;

    case 2:
        md_nsq_motion_search_ctrls->enabled = 1;
        md_nsq_motion_search_ctrls->use_ssd = 0;
        md_nsq_motion_search_ctrls->full_pel_search_width = 15;
        md_nsq_motion_search_ctrls->full_pel_search_height = 15;
        break;
    case 3:
        md_nsq_motion_search_ctrls->enabled = 1;
        md_nsq_motion_search_ctrls->use_ssd = 0;
        md_nsq_motion_search_ctrls->full_pel_search_width = 11;
        md_nsq_motion_search_ctrls->full_pel_search_height = 11;
        break;
    case 4:
        md_nsq_motion_search_ctrls->enabled = 1;
        md_nsq_motion_search_ctrls->use_ssd = 0;
        md_nsq_motion_search_ctrls->full_pel_search_width = 7;
        md_nsq_motion_search_ctrls->full_pel_search_height = 7;
        break;
    default:
        assert(0);
        break;
    }
}
void md_pme_search_controls(ModeDecisionContext *mdctxt, uint8_t md_pme_level) {

    MdPmeCtrls *md_pme_ctrls = &mdctxt->md_pme_ctrls;

    switch (md_pme_level)
    {
    case 0:
        md_pme_ctrls->enabled = 0;
        break;

    case 1:
        md_pme_ctrls->enabled = 1;
        md_pme_ctrls->use_ssd = 1;
        md_pme_ctrls->full_pel_search_width = 15;
        md_pme_ctrls->full_pel_search_height = 15;
        md_pme_ctrls->pre_fp_pme_to_me_cost_th = MAX_SIGNED_VALUE;
        md_pme_ctrls->pre_fp_pme_to_me_mv_th = MIN_SIGNED_VALUE;
        md_pme_ctrls->post_fp_pme_to_me_cost_th = MAX_SIGNED_VALUE;
        md_pme_ctrls->post_fp_pme_to_me_mv_th = MIN_SIGNED_VALUE;
        break;

    case 2:
        md_pme_ctrls->enabled = 1;
        md_pme_ctrls->use_ssd = 1;
        md_pme_ctrls->full_pel_search_width = 7;
        md_pme_ctrls->full_pel_search_height = 5;
        md_pme_ctrls->pre_fp_pme_to_me_cost_th = MAX_SIGNED_VALUE;
        md_pme_ctrls->pre_fp_pme_to_me_mv_th = MIN_SIGNED_VALUE;
        md_pme_ctrls->post_fp_pme_to_me_cost_th = MAX_SIGNED_VALUE;
        md_pme_ctrls->post_fp_pme_to_me_mv_th = MIN_SIGNED_VALUE;
        break;

    case 3:
        md_pme_ctrls->enabled = 1;
        md_pme_ctrls->use_ssd = 1;
        md_pme_ctrls->full_pel_search_width = 7;
        md_pme_ctrls->full_pel_search_height = 5;
        md_pme_ctrls->pre_fp_pme_to_me_cost_th = 100;
        md_pme_ctrls->pre_fp_pme_to_me_mv_th = 16;
        md_pme_ctrls->post_fp_pme_to_me_cost_th = 25;
        md_pme_ctrls->post_fp_pme_to_me_mv_th = 32;
        break;
#if TUNE_M9_PME
    case 4:
        md_pme_ctrls->enabled = 1;
        md_pme_ctrls->use_ssd = 0;
        md_pme_ctrls->full_pel_search_width = 3;
        md_pme_ctrls->full_pel_search_height = 3;
        md_pme_ctrls->pre_fp_pme_to_me_cost_th = 25;
        md_pme_ctrls->pre_fp_pme_to_me_mv_th = 16;
        md_pme_ctrls->post_fp_pme_to_me_cost_th = 5;
        md_pme_ctrls->post_fp_pme_to_me_mv_th = 32;
        break;
#endif
    default:
        assert(0);
        break;
    }
}
void set_pf_controls(ModeDecisionContext *mdctxt, uint8_t pf_level) {

   PfCtrls *pf_ctrls = &mdctxt->pf_ctrls;

    switch (pf_level) {
    case 0:
        pf_ctrls->pf_shape = ONLY_DC_SHAPE;
        break;
    case 1:
        pf_ctrls->pf_shape = DEFAULT_SHAPE;
        break;
    case 2:
        pf_ctrls->pf_shape = N2_SHAPE;
        break;
    case 3:
        pf_ctrls->pf_shape = N4_SHAPE;
        break;
    default:
        assert(0);
        break;
    }
}

#if OPT_REFACTOR_IN_DEPTH_CTRLS
/*
 * Control in-depth block skip
 */
void set_in_depth_block_skip_ctrls(ModeDecisionContext *mdctxt, uint8_t in_depth_block_skip_level) {

    InDepthBlockSkipCtrls *in_depth_block_skip_ctrls = &mdctxt->in_depth_block_skip_ctrls;

    switch (in_depth_block_skip_level)
    {
    case 0:
        in_depth_block_skip_ctrls->base_weight                 =   0;
        break;

    case 1:
        in_depth_block_skip_ctrls->base_weight                 = 150;

        in_depth_block_skip_ctrls->cost_band_based_modulation  =   1;
        in_depth_block_skip_ctrls->max_cost_multiplier         = 400;
        in_depth_block_skip_ctrls->max_band_cnt                =   5;
        in_depth_block_skip_ctrls->weight_per_band[0]          = 175;
        in_depth_block_skip_ctrls->weight_per_band[1]          = 150;
        in_depth_block_skip_ctrls->weight_per_band[2]          = 125;
        in_depth_block_skip_ctrls->weight_per_band[3]          = 100;
        in_depth_block_skip_ctrls->weight_per_band[4]          =  75;

        in_depth_block_skip_ctrls->child_cnt_based_modulation  =   0;
        in_depth_block_skip_ctrls->cnt_based_weight[0]         = 150;
        in_depth_block_skip_ctrls->cnt_based_weight[1]         = 125;
        in_depth_block_skip_ctrls->cnt_based_weight[2]         = 100;
        break;

    case 2:
        in_depth_block_skip_ctrls->base_weight                 = 150;

        in_depth_block_skip_ctrls->cost_band_based_modulation  =   0;

        in_depth_block_skip_ctrls->child_cnt_based_modulation  =   0;
        break;

    default:
        assert(0);
        break;
    }
}
#endif
#if LOWER_DEPTH_EXIT_CTRL
/*
 * Control lower-depth block skip
 */
void set_lower_depth_block_skip_ctrls(ModeDecisionContext *mdctxt, uint8_t lower_depth_block_skip_level) {

    LowerDepthBlockSkipCtrls *lower_depth_skip_ctrls = &mdctxt->lower_depth_block_skip_ctrls;

    switch (lower_depth_block_skip_level)
    {
    case 0:
        lower_depth_skip_ctrls->enabled             = 0;
        break;

    case 1:
        lower_depth_skip_ctrls->enabled                   =   1;
        lower_depth_skip_ctrls->quad_deviation_th         = 500;
        lower_depth_skip_ctrls->min_distortion_cost_ratio =  50;
        lower_depth_skip_ctrls->skip_all                  =   0;
        break;

    case 2:
        lower_depth_skip_ctrls->enabled                   =   1;
        lower_depth_skip_ctrls->quad_deviation_th         = 500;
        lower_depth_skip_ctrls->min_distortion_cost_ratio =  50;
        lower_depth_skip_ctrls->skip_all                  =   1;
        break;

    default:
        assert(0);
        break;
    }
}
#else
#if FTR_IMPROVE_DEPTH_REMOVAL
/*
 * Control Depth Reduction
 */
void set_depth_skip_controls(ModeDecisionContext *mdctxt, uint8_t depth_skip_level) {

    DepthSkipCtrls *depth_skip_ctrls = &mdctxt->depth_skip_ctrls;

    switch (depth_skip_level)
    {
    case 0:
        depth_skip_ctrls->enabled = 0;
        break;

    case 1:
        depth_skip_ctrls->enabled = 1;
        depth_skip_ctrls->quand_deviation_th = 500;
        break;

    case 2:
        depth_skip_ctrls->enabled = 1;
        depth_skip_ctrls->quand_deviation_th = 1000;
        break;

    case 3:
        depth_skip_ctrls->enabled = 1;
        depth_skip_ctrls->quand_deviation_th = 1500;
        break;

    default:
        assert(0);
        break;
    }
}
#endif
#endif
/*
 * Control Adaptive ME search
 */
void md_sq_motion_search_controls(ModeDecisionContext *mdctxt, uint8_t md_sq_mv_search_level) {

    MdSqMotionSearchCtrls *md_sq_me_ctrls = &mdctxt->md_sq_me_ctrls;

    switch (md_sq_mv_search_level) {
    case 0:
        md_sq_me_ctrls->enabled = 0;
        break;
    case 1:
        md_sq_me_ctrls->enabled = 1;
        md_sq_me_ctrls->use_ssd = 0;

        md_sq_me_ctrls->pame_distortion_th = 10;

        md_sq_me_ctrls->sprs_lev0_enabled = 1;
        md_sq_me_ctrls->sprs_lev0_step = 4;
        md_sq_me_ctrls->sprs_lev0_w = 15;
        md_sq_me_ctrls->sprs_lev0_h = 15;
        md_sq_me_ctrls->max_sprs_lev0_w = 150;
        md_sq_me_ctrls->max_sprs_lev0_h = 150;
        md_sq_me_ctrls->sprs_lev0_multiplier = 500;

        md_sq_me_ctrls->sprs_lev1_enabled = 1;
        md_sq_me_ctrls->sprs_lev1_step = 2;
        md_sq_me_ctrls->sprs_lev1_w = 4;
        md_sq_me_ctrls->sprs_lev1_h = 4;
        md_sq_me_ctrls->max_sprs_lev1_w = 50;
        md_sq_me_ctrls->max_sprs_lev1_h = 50;
        md_sq_me_ctrls->sprs_lev1_multiplier = 500;

        md_sq_me_ctrls->sprs_lev2_enabled = 1;
        md_sq_me_ctrls->sprs_lev2_step = 1;
        md_sq_me_ctrls->sprs_lev2_w = 3;
        md_sq_me_ctrls->sprs_lev2_h = 3;
        break;
    case 2:
        md_sq_me_ctrls->enabled = 1;
        md_sq_me_ctrls->use_ssd = 0;

        md_sq_me_ctrls->pame_distortion_th = 10;

        md_sq_me_ctrls->sprs_lev0_enabled = 1;
        md_sq_me_ctrls->sprs_lev0_step = 4;
        md_sq_me_ctrls->sprs_lev0_w = 15;
        md_sq_me_ctrls->sprs_lev0_h = 15;
        md_sq_me_ctrls->max_sprs_lev0_w = 150;
        md_sq_me_ctrls->max_sprs_lev0_h = 150;
        md_sq_me_ctrls->sprs_lev0_multiplier = 400;

        md_sq_me_ctrls->sprs_lev1_enabled = 1;
        md_sq_me_ctrls->sprs_lev1_step = 2;
        md_sq_me_ctrls->sprs_lev1_w = 4;
        md_sq_me_ctrls->sprs_lev1_h = 4;
        md_sq_me_ctrls->max_sprs_lev1_w = 50;
        md_sq_me_ctrls->max_sprs_lev1_h = 50;
        md_sq_me_ctrls->sprs_lev1_multiplier = 400;

        md_sq_me_ctrls->sprs_lev2_enabled = 1;
        md_sq_me_ctrls->sprs_lev2_step = 1;
        md_sq_me_ctrls->sprs_lev2_w = 3;
        md_sq_me_ctrls->sprs_lev2_h = 3;
        break;
    case 3:
        md_sq_me_ctrls->enabled = 1;
        md_sq_me_ctrls->use_ssd = 0;

        md_sq_me_ctrls->pame_distortion_th = 10;

        md_sq_me_ctrls->sprs_lev0_enabled = 1;
        md_sq_me_ctrls->sprs_lev0_step = 4;
        md_sq_me_ctrls->sprs_lev0_w = 15;
        md_sq_me_ctrls->sprs_lev0_h = 15;
        md_sq_me_ctrls->max_sprs_lev0_w = 150;
        md_sq_me_ctrls->max_sprs_lev0_h = 150;
        md_sq_me_ctrls->sprs_lev0_multiplier = 300;

        md_sq_me_ctrls->sprs_lev1_enabled = 1;
        md_sq_me_ctrls->sprs_lev1_step = 2;
        md_sq_me_ctrls->sprs_lev1_w = 4;
        md_sq_me_ctrls->sprs_lev1_h = 4;
        md_sq_me_ctrls->max_sprs_lev1_w = 50;
        md_sq_me_ctrls->max_sprs_lev1_h = 50;
        md_sq_me_ctrls->sprs_lev1_multiplier = 300;

        md_sq_me_ctrls->sprs_lev2_enabled = 1;
        md_sq_me_ctrls->sprs_lev2_step = 1;
        md_sq_me_ctrls->sprs_lev2_w = 3;
        md_sq_me_ctrls->sprs_lev2_h = 3;
        break;
    case 4:
        md_sq_me_ctrls->enabled = 1;
        md_sq_me_ctrls->use_ssd = 0;
        md_sq_me_ctrls->pame_distortion_th = 10;

        md_sq_me_ctrls->sprs_lev0_enabled = 1;
        md_sq_me_ctrls->sprs_lev0_step = 4;
        md_sq_me_ctrls->sprs_lev0_w = 15;
        md_sq_me_ctrls->sprs_lev0_h = 15;
        md_sq_me_ctrls->max_sprs_lev0_w = 150;
        md_sq_me_ctrls->max_sprs_lev0_h = 150;
        md_sq_me_ctrls->sprs_lev0_multiplier = 100;

        md_sq_me_ctrls->sprs_lev1_enabled = 1;
        md_sq_me_ctrls->sprs_lev1_step = 2;
        md_sq_me_ctrls->sprs_lev1_w = 4;
        md_sq_me_ctrls->sprs_lev1_h = 4;
        md_sq_me_ctrls->max_sprs_lev1_w = 50;
        md_sq_me_ctrls->max_sprs_lev1_h = 50;
        md_sq_me_ctrls->sprs_lev1_multiplier = 100;

        md_sq_me_ctrls->sprs_lev2_enabled = 1;
        md_sq_me_ctrls->sprs_lev2_step = 1;
        md_sq_me_ctrls->sprs_lev2_w = 3;
        md_sq_me_ctrls->sprs_lev2_h = 3;
        break;
    default:
        assert(0);
        break;
    }
}
/*
 * Control Subpel search of ME MV(s)
 */
void md_subpel_me_controls(ModeDecisionContext *mdctxt, uint8_t md_subpel_me_level) {
    MdSubPelSearchCtrls *md_subpel_me_ctrls = &mdctxt->md_subpel_me_ctrls;

    switch (md_subpel_me_level) {
    case 0:
        md_subpel_me_ctrls->enabled = 0;
        break;
    case 1:
        md_subpel_me_ctrls->enabled = 1;
        md_subpel_me_ctrls->subpel_search_type = USE_8_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 2;
        md_subpel_me_ctrls->eight_pel_search_enabled = 1;
#if FTR_PRUNED_SUBPEL_TREE
        md_subpel_me_ctrls->subpel_search_method = SUBPEL_TREE;
#endif
        break;
    case 2:
        md_subpel_me_ctrls->enabled = 1;
        md_subpel_me_ctrls->subpel_search_type = USE_4_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 2;
        md_subpel_me_ctrls->eight_pel_search_enabled = 0;
#if FTR_PRUNED_SUBPEL_TREE
        md_subpel_me_ctrls->subpel_search_method = SUBPEL_TREE;
#endif
        break;
    case 3:
        md_subpel_me_ctrls->enabled = 1;
        md_subpel_me_ctrls->subpel_search_type = USE_4_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 1;
        md_subpel_me_ctrls->eight_pel_search_enabled = 0;
#if FTR_PRUNED_SUBPEL_TREE
        md_subpel_me_ctrls->subpel_search_method = SUBPEL_TREE;
#endif
        break;
#if FTR_PRUNED_SUBPEL_TREE
    case 4:
        md_subpel_me_ctrls->enabled = 1;
        md_subpel_me_ctrls->subpel_search_type = USE_8_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 2;
        md_subpel_me_ctrls->eight_pel_search_enabled = 1;
        md_subpel_me_ctrls->subpel_search_method = SUBPEL_TREE_PRUNED;
        break;
    case 5:
        md_subpel_me_ctrls->enabled = 1;
        md_subpel_me_ctrls->subpel_search_type = USE_4_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 2;
        md_subpel_me_ctrls->eight_pel_search_enabled = 0;
        md_subpel_me_ctrls->subpel_search_method = SUBPEL_TREE_PRUNED;
        break;
#if TUNE_M10_SUBPEL
    case 6:
        md_subpel_me_ctrls->enabled = 1;
        md_subpel_me_ctrls->subpel_search_type = USE_4_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 1;
        md_subpel_me_ctrls->eight_pel_search_enabled = 0;
        md_subpel_me_ctrls->subpel_search_method = SUBPEL_TREE_PRUNED;
        break;
#endif
#endif
    default: assert(0); break;
    }
}

/*
 * Control Subpel search of PME MV(s)
 */
void md_subpel_pme_controls(ModeDecisionContext *mdctxt, uint8_t md_subpel_pme_level) {
    MdSubPelSearchCtrls *md_subpel_pme_ctrls = &mdctxt->md_subpel_pme_ctrls;

    switch (md_subpel_pme_level) {
    case 0:
        md_subpel_pme_ctrls->enabled = 0;
        break;
    case 1:
        md_subpel_pme_ctrls->enabled = 1;
        md_subpel_pme_ctrls->subpel_search_type = USE_8_TAPS;
        md_subpel_pme_ctrls->subpel_iters_per_step = 2;
        md_subpel_pme_ctrls->eight_pel_search_enabled = 1;
#if FTR_PRUNED_SUBPEL_TREE
        md_subpel_pme_ctrls->subpel_search_method = SUBPEL_TREE;
#endif
        break;
#if FTR_PRUNED_SUBPEL_TREE
    case 2:
        md_subpel_pme_ctrls->enabled = 1;
        md_subpel_pme_ctrls->subpel_search_type = USE_8_TAPS;
        md_subpel_pme_ctrls->subpel_iters_per_step = 2;
        md_subpel_pme_ctrls->eight_pel_search_enabled = 1;
        md_subpel_pme_ctrls->subpel_search_method = SUBPEL_TREE_PRUNED;
        break;
    case 3:
        md_subpel_pme_ctrls->enabled = 1;
        md_subpel_pme_ctrls->subpel_search_type = USE_4_TAPS;
        md_subpel_pme_ctrls->subpel_iters_per_step = 1;
        md_subpel_pme_ctrls->eight_pel_search_enabled = 0;
        md_subpel_pme_ctrls->subpel_search_method = SUBPEL_TREE;
        break;
#else
    case 2:
        md_subpel_pme_ctrls->enabled = 1;
        md_subpel_pme_ctrls->subpel_search_type = USE_4_TAPS;
        md_subpel_pme_ctrls->subpel_iters_per_step = 2;
        md_subpel_pme_ctrls->eight_pel_search_enabled = 0;
        break;
    case 3:
        md_subpel_pme_ctrls->enabled = 1;
        md_subpel_pme_ctrls->subpel_search_type = USE_4_TAPS;
        md_subpel_pme_ctrls->subpel_iters_per_step = 1;
        md_subpel_pme_ctrls->eight_pel_search_enabled = 0;
        break;
#endif
    default: assert(0); break;
    }
}
#if CLN_CANDIDATE_ELEMINATION_CTR
void set_cand_elimination_controls(ModeDecisionContext *mdctxt, uint8_t eliminate_candidate_based_on_pme_me_results){
    CandEliminationCtlrs *cand_elimination_ctrls = &mdctxt->cand_elimination_ctrs;
    switch (eliminate_candidate_based_on_pme_me_results) {
    case 0:
        cand_elimination_ctrls->enabled = 0;
        break;
    case 1:
        cand_elimination_ctrls->enabled = 1;
        cand_elimination_ctrls->dc_only = 1;
        cand_elimination_ctrls->inject_new_me = 1;
        cand_elimination_ctrls->inject_new_pme = 1;
        cand_elimination_ctrls->inject_new_warp = 1;
        break;
#if OPT_WM
    case 2:
        cand_elimination_ctrls->enabled = 1;
        cand_elimination_ctrls->dc_only = 1;
        cand_elimination_ctrls->inject_new_me = 1;
        cand_elimination_ctrls->inject_new_pme = 1;
        cand_elimination_ctrls->inject_new_warp = 2;
        break;
#endif
    default: assert(0); break;
    }
}
#endif
#if !FTR_NEW_CYCLES_ALLOC
void coeff_based_switch_md_controls(ModeDecisionContext *mdctxt, uint8_t switch_md_mode_based_on_sq_coeff_level) {
    CoeffBSwMdCtrls *coeffb_sw_md_ctrls = &mdctxt->cb_sw_md_ctrls;

    switch (switch_md_mode_based_on_sq_coeff_level) {
    case 0: coeffb_sw_md_ctrls->enabled = 0; break;
    case 1:
        coeffb_sw_md_ctrls->enabled = 1;
        coeffb_sw_md_ctrls->non_skip_level = 0;
        coeffb_sw_md_ctrls->skip_block = 0;
        break;
    case 2:
        coeffb_sw_md_ctrls->enabled = 1;
        coeffb_sw_md_ctrls->non_skip_level = 1;
        coeffb_sw_md_ctrls->skip_block = 0;
        break;
    case 3:
        coeffb_sw_md_ctrls->enabled = 1;
        coeffb_sw_md_ctrls->non_skip_level = 1;
        coeffb_sw_md_ctrls->skip_block = 1;
        break;
    default: assert(0); break;
    }
}
#endif
/*
 * Control RDOQ
 */
void set_rdoq_controls(ModeDecisionContext *mdctxt, uint8_t rdoq_level) {
    RdoqCtrls *rdoq_ctrls = &mdctxt->rdoq_ctrls;

    switch (rdoq_level) {
    case 0:
        rdoq_ctrls->enabled = 0;
        break;
    case 1:
        rdoq_ctrls->enabled = 1;
        rdoq_ctrls->eob_fast_l_inter = 0;
        rdoq_ctrls->eob_fast_l_intra = 0;
#if TUNE_CHROMA_SSIM
        rdoq_ctrls->eob_fast_c_inter = 0;
#else
        rdoq_ctrls->eob_fast_c_inter = 1;
#endif
        rdoq_ctrls->eob_fast_c_intra = 0;
        rdoq_ctrls->fp_q_l = 1;
        rdoq_ctrls->fp_q_c = 1;
        rdoq_ctrls->satd_factor = (uint8_t)~0;
        rdoq_ctrls->early_exit_th = 0;
#if OPT_RDOQ_FOR_M9
        rdoq_ctrls->disallow_md_rdoq_uv = 0;
        rdoq_ctrls->md_satd_factor = (uint8_t)~0;
#endif
        break;
    case 2:
        rdoq_ctrls->enabled = 1;
        rdoq_ctrls->eob_fast_l_inter = 0;
        rdoq_ctrls->eob_fast_l_intra = 0;
#if TUNE_CHROMA_SSIM
        rdoq_ctrls->eob_fast_c_inter = 0;
#else
        rdoq_ctrls->eob_fast_c_inter = 1;
#endif
        rdoq_ctrls->eob_fast_c_intra = 0;
        rdoq_ctrls->fp_q_l = 1;
        rdoq_ctrls->fp_q_c = 0;
        rdoq_ctrls->satd_factor = 128;
        rdoq_ctrls->early_exit_th = 5;
#if OPT_RDOQ_FOR_M9
        rdoq_ctrls->disallow_md_rdoq_uv = 1;
        rdoq_ctrls->md_satd_factor = 64;
#endif
        break;
    case 3:
        rdoq_ctrls->enabled = 1;
        rdoq_ctrls->eob_fast_l_inter = 0;
        rdoq_ctrls->eob_fast_l_intra = 0;
#if TUNE_CHROMA_SSIM
        rdoq_ctrls->eob_fast_c_inter = 0;
#else
        rdoq_ctrls->eob_fast_c_inter = 1;
#endif
        rdoq_ctrls->eob_fast_c_intra = 0;
        rdoq_ctrls->fp_q_l = 1;
        rdoq_ctrls->fp_q_c = 0;
#if OPT_RDOQ_FOR_M9
        rdoq_ctrls->satd_factor = 128;
#else
        rdoq_ctrls->satd_factor = 64;
#endif
        rdoq_ctrls->early_exit_th = 5;
#if OPT_RDOQ_FOR_M9
        rdoq_ctrls->disallow_md_rdoq_uv = 1;
        rdoq_ctrls->md_satd_factor = 32;
#endif
        break;
    default: assert(0); break;
    }
}
#if !FTR_NEW_CYCLES_ALLOC
/******************************************************
* Derive SB classifier thresholds
******************************************************/
uint8_t nsq_cycles_reduction_th[19] = {
 0, // NONE
 17, //[85%;100%]
 15,//[75%;85%]
 14,//[65%;75%]
 13,//[60%;65%]
 12,//[55%;60%]
 11,//[50%;65%]
 10,//[45%;50%]
 9,//[40%;45%]
 8,//[35%;40%]
 7,//[30%;35%]
 6,//[25%;30%]
 6,//[20%;25%]
 5,//[17%;20%]
 5,//[14%;17%]
 4,//[10%;14%]
 3,//[6%;10%]
 2,//[3%;6%]
 1 //[0%;3%]
};
#endif
#if !CLN_NSQ_AND_STATS
void adaptive_md_cycles_redcution_controls(ModeDecisionContext *mdctxt, uint8_t adaptive_md_cycles_red_mode) {
    AMdCycleRControls* adaptive_md_cycles_red_ctrls = &mdctxt->admd_cycles_red_ctrls;
    switch (adaptive_md_cycles_red_mode)
    {
    case 0:
        adaptive_md_cycles_red_ctrls->enabled = 0;
        adaptive_md_cycles_red_ctrls->skip_nsq_th = 0;
        adaptive_md_cycles_red_ctrls->switch_level_th = 0;
        adaptive_md_cycles_red_ctrls->non_skip_level = 0;
        break;
    case 1:
        adaptive_md_cycles_red_ctrls->enabled = 1;
        adaptive_md_cycles_red_ctrls->skip_nsq_th = 25;
        adaptive_md_cycles_red_ctrls->switch_level_th = 0;
        adaptive_md_cycles_red_ctrls->non_skip_level = 0;
        break;
    case 2:
        adaptive_md_cycles_red_ctrls->enabled = 1;
        adaptive_md_cycles_red_ctrls->skip_nsq_th = 100;
        adaptive_md_cycles_red_ctrls->switch_level_th = 0;
        adaptive_md_cycles_red_ctrls->non_skip_level = 0;
        break;
    case 3:
        adaptive_md_cycles_red_ctrls->enabled = 1;
        adaptive_md_cycles_red_ctrls->skip_nsq_th = 150;
        adaptive_md_cycles_red_ctrls->switch_level_th = 0;
        adaptive_md_cycles_red_ctrls->non_skip_level = 0;
        break;
    case 4:
        adaptive_md_cycles_red_ctrls->enabled = 1;
        adaptive_md_cycles_red_ctrls->skip_nsq_th = 300;
        adaptive_md_cycles_red_ctrls->switch_level_th = 600;
        adaptive_md_cycles_red_ctrls->non_skip_level = 1;
        break;
    case 5:
        adaptive_md_cycles_red_ctrls->enabled = 1;
        adaptive_md_cycles_red_ctrls->skip_nsq_th = 300;
        adaptive_md_cycles_red_ctrls->switch_level_th = 750;
        adaptive_md_cycles_red_ctrls->non_skip_level = 0;
        break;
    case 6:
        adaptive_md_cycles_red_ctrls->enabled = 1;
        adaptive_md_cycles_red_ctrls->skip_nsq_th = 500;
        adaptive_md_cycles_red_ctrls->switch_level_th = 1000;
        adaptive_md_cycles_red_ctrls->non_skip_level = 1;
        break;
    case 7:
        adaptive_md_cycles_red_ctrls->enabled = 1;
        adaptive_md_cycles_red_ctrls->skip_nsq_th = 750;
        adaptive_md_cycles_red_ctrls->switch_level_th = 1500;
        adaptive_md_cycles_red_ctrls->non_skip_level = 1;
        break;
    default:
        assert(0);
        break;
    }
}
#endif
#if FTR_NEW_CYCLES_ALLOC
/*
 * Settings for the parent SQ coeff-area based cycles reduction algorithm.
 */
void set_parent_sq_coeff_area_based_cycles_reduction_ctrls(ModeDecisionContext* ctx, uint8_t resolution, uint8_t cycles_alloc_lvl) {
    ParentSqCoeffAreaBasedCyclesReductionCtrls* cycle_red_ctrls = &ctx->parent_sq_coeff_area_based_cycles_reduction_ctrls;
    switch (cycles_alloc_lvl) {
    case 0:
        cycle_red_ctrls->enabled = 0;
        break;
    case 1:
        cycle_red_ctrls->enabled = 1;

        // High frequency band THs/actions
        cycle_red_ctrls->high_freq_band1_th = resolution <= INPUT_SIZE_360p_RANGE ? UNUSED_HIGH_FREQ_BAND_TH : 90;
        cycle_red_ctrls->high_freq_band1_level = 3;
        cycle_red_ctrls->high_freq_band2_th = resolution <= INPUT_SIZE_360p_RANGE ? UNUSED_HIGH_FREQ_BAND_TH : 70;
        cycle_red_ctrls->high_freq_band2_level = 2;
#if TUNE_LOWER_PRESETS
        cycle_red_ctrls->high_freq_band3_th = UNUSED_HIGH_FREQ_BAND_TH;
        cycle_red_ctrls->high_freq_band3_level = 0;
#endif

        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action = 1;
        cycle_red_ctrls->enable_one_coeff_action = 0;
        cycle_red_ctrls->one_coeff_action = 0;

        cycle_red_ctrls->low_freq_band1_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band1_level = 0;
        cycle_red_ctrls->low_freq_band2_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band2_level = 0;
        break;
    case 2:
        cycle_red_ctrls->enabled = 1;

        // High frequency band THs/actions
        cycle_red_ctrls->high_freq_band1_th = resolution <= INPUT_SIZE_360p_RANGE ? UNUSED_HIGH_FREQ_BAND_TH : 90;
        cycle_red_ctrls->high_freq_band1_level = 3;
        cycle_red_ctrls->high_freq_band2_th = resolution <= INPUT_SIZE_360p_RANGE ? UNUSED_HIGH_FREQ_BAND_TH : 70;
        cycle_red_ctrls->high_freq_band2_level = 2;
#if TUNE_LOWER_PRESETS
        cycle_red_ctrls->high_freq_band3_th = UNUSED_HIGH_FREQ_BAND_TH;
        cycle_red_ctrls->high_freq_band3_level = 0;
#endif

        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action = 2;
        cycle_red_ctrls->enable_one_coeff_action = 0;
        cycle_red_ctrls->one_coeff_action = 0;

        cycle_red_ctrls->low_freq_band1_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band1_level = 0;
        cycle_red_ctrls->low_freq_band2_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band2_level = 0;
        break;
    case 3:
        cycle_red_ctrls->enabled = 1;

        // High frequency band THs/actions
#if TUNE_LOWER_PRESETS
        cycle_red_ctrls->high_freq_band1_th = 90;
        cycle_red_ctrls->high_freq_band1_level = resolution <= INPUT_SIZE_360p_RANGE ? 2 : 3;
        cycle_red_ctrls->high_freq_band2_th = 70;
        cycle_red_ctrls->high_freq_band2_level = resolution <= INPUT_SIZE_360p_RANGE ? 1 : 3;
        cycle_red_ctrls->high_freq_band3_th = UNUSED_HIGH_FREQ_BAND_TH;
        cycle_red_ctrls->high_freq_band3_level = 0;
#else
        cycle_red_ctrls->high_freq_band1_th = resolution <= INPUT_SIZE_360p_RANGE ? UNUSED_HIGH_FREQ_BAND_TH : 90;
        cycle_red_ctrls->high_freq_band1_level = 3;
        cycle_red_ctrls->high_freq_band2_th = resolution <= INPUT_SIZE_360p_RANGE ? UNUSED_HIGH_FREQ_BAND_TH : 70;
        cycle_red_ctrls->high_freq_band2_level = 3;
#endif

        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action = 2;
        cycle_red_ctrls->enable_one_coeff_action = 0;
        cycle_red_ctrls->one_coeff_action = 0;

        cycle_red_ctrls->low_freq_band1_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band1_level = 0;
        cycle_red_ctrls->low_freq_band2_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band2_level = 0;
        break;
#if TUNE_M4_M8
    case 4:
        cycle_red_ctrls->enabled = 1;

        // High frequency band THs/actions
        cycle_red_ctrls->high_freq_band1_th = 90;
        cycle_red_ctrls->high_freq_band1_level = resolution <= INPUT_SIZE_360p_RANGE ? 2 : 3;
        cycle_red_ctrls->high_freq_band2_th = 70;
        cycle_red_ctrls->high_freq_band2_level = resolution <= INPUT_SIZE_360p_RANGE ? 1 : 3;
        cycle_red_ctrls->high_freq_band3_th = UNUSED_HIGH_FREQ_BAND_TH;
        cycle_red_ctrls->high_freq_band3_level = 0;

        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action = 3;
        cycle_red_ctrls->enable_one_coeff_action = 1;
        cycle_red_ctrls->one_coeff_action = 1;

        cycle_red_ctrls->low_freq_band1_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band1_level = 0;
        cycle_red_ctrls->low_freq_band2_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band2_level = 0;
        break;
    case 5:
        cycle_red_ctrls->enabled = 1;

        // High frequency band THs/actions
        cycle_red_ctrls->high_freq_band1_th = 90;
        cycle_red_ctrls->high_freq_band1_level = 3;
        cycle_red_ctrls->high_freq_band2_th = 70;
        cycle_red_ctrls->high_freq_band2_level = 2;
        cycle_red_ctrls->high_freq_band3_th = 50;
        cycle_red_ctrls->high_freq_band3_level = 1;


        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action = 2;
        cycle_red_ctrls->enable_one_coeff_action = 0;
        cycle_red_ctrls->one_coeff_action = 0;

        cycle_red_ctrls->low_freq_band1_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band1_level = 0;
        cycle_red_ctrls->low_freq_band2_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band2_level = 0;
        break;
    case 6:
        cycle_red_ctrls->enabled = 1;

        // High frequency band THs/actions
        cycle_red_ctrls->high_freq_band1_th = 90;
        cycle_red_ctrls->high_freq_band1_level = resolution <= INPUT_SIZE_360p_RANGE ? 2 : 3;
        cycle_red_ctrls->high_freq_band2_th = 70;
        cycle_red_ctrls->high_freq_band2_level = resolution <= INPUT_SIZE_360p_RANGE ? 1 : 3;
        cycle_red_ctrls->high_freq_band3_th = UNUSED_HIGH_FREQ_BAND_TH;
        cycle_red_ctrls->high_freq_band3_level = 0;

        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action = 0;
        cycle_red_ctrls->enable_one_coeff_action = 1;
        cycle_red_ctrls->one_coeff_action = 1;

        cycle_red_ctrls->low_freq_band1_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band1_level = 0;
        cycle_red_ctrls->low_freq_band2_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band2_level = 0;
        break;

    case 7:
        cycle_red_ctrls->enabled = 1;

        // High frequency band THs/actions
        cycle_red_ctrls->high_freq_band1_th = 90;
        cycle_red_ctrls->high_freq_band1_level = 0;
        cycle_red_ctrls->high_freq_band2_th = 70;
        cycle_red_ctrls->high_freq_band2_level = 3;
        cycle_red_ctrls->high_freq_band3_th = 50;
        cycle_red_ctrls->high_freq_band3_level = 2;

        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action = 0;
        cycle_red_ctrls->enable_one_coeff_action = 1;
        cycle_red_ctrls->one_coeff_action = 1;

        cycle_red_ctrls->low_freq_band1_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band1_level = 0;
        cycle_red_ctrls->low_freq_band2_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band2_level = 0;
        break;
#else
#if TUNE_LOWER_PRESETS
    case 4:
        cycle_red_ctrls->enabled = 1;

        // High frequency band THs/actions
        cycle_red_ctrls->high_freq_band1_th = 90;
        cycle_red_ctrls->high_freq_band1_level = resolution <= INPUT_SIZE_360p_RANGE ? 2 : 3;
        cycle_red_ctrls->high_freq_band2_th = 70;
        cycle_red_ctrls->high_freq_band2_level = resolution <= INPUT_SIZE_360p_RANGE ? 1 : 3;
        cycle_red_ctrls->high_freq_band3_th = UNUSED_HIGH_FREQ_BAND_TH;
        cycle_red_ctrls->high_freq_band3_level = 0;

        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action = 3;
        cycle_red_ctrls->enable_one_coeff_action = 1;
        cycle_red_ctrls->one_coeff_action = 1;

        cycle_red_ctrls->low_freq_band1_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band1_level = 0;
        cycle_red_ctrls->low_freq_band2_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band2_level = 0;
        break;

    case 5:
        cycle_red_ctrls->enabled = 1;

        // High frequency band THs/actions
        cycle_red_ctrls->high_freq_band1_th = 90;
        cycle_red_ctrls->high_freq_band1_level = 0;
        cycle_red_ctrls->high_freq_band2_th = 70;
        cycle_red_ctrls->high_freq_band2_level = 3;
        cycle_red_ctrls->high_freq_band3_th = 50;
        cycle_red_ctrls->high_freq_band3_level = 2;

        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action = 0;
        cycle_red_ctrls->enable_one_coeff_action = 1;
        cycle_red_ctrls->one_coeff_action = 1;

        cycle_red_ctrls->low_freq_band1_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band1_level = 0;
        cycle_red_ctrls->low_freq_band2_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band2_level = 0;
        break;
#else
    case 4:
        cycle_red_ctrls->enabled = 1;

        // High frequency band THs/actions
        cycle_red_ctrls->high_freq_band1_th = resolution <= INPUT_SIZE_360p_RANGE ? UNUSED_HIGH_FREQ_BAND_TH : 90;
        cycle_red_ctrls->high_freq_band1_level = 3;
        cycle_red_ctrls->high_freq_band2_th = resolution <= INPUT_SIZE_360p_RANGE ? UNUSED_HIGH_FREQ_BAND_TH : 70;
        cycle_red_ctrls->high_freq_band2_level = 3;

        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action = 0; // skip
        cycle_red_ctrls->enable_one_coeff_action = 0;
        cycle_red_ctrls->one_coeff_action = 0;

        cycle_red_ctrls->low_freq_band1_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band1_level = 0;
        cycle_red_ctrls->low_freq_band2_th = UNUSED_LOW_FREQ_BAND_TH;
        cycle_red_ctrls->low_freq_band2_level = 0;
        break;
#endif
#endif
    default:
        assert(0);
        break;
    }
}
#endif
void set_txt_controls(ModeDecisionContext *mdctxt, uint8_t txt_level) {

    TxtControls * txt_ctrls = &mdctxt->txt_ctrls;

    switch (txt_level)
    {
    case 0:
        txt_ctrls->enabled = 0;

        txt_ctrls->txt_group_inter_lt_16x16 = 1;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = 1;

        txt_ctrls->txt_group_intra_lt_16x16 = 1;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = 1;
#if !TUNE_REMOVE_TXT_STATS
        txt_ctrls->use_stats = 0;
        txt_ctrls->intra_th = 0;
        txt_ctrls->inter_th = 0;
#endif
        break;
    case 1:
        txt_ctrls->enabled = 1;

        txt_ctrls->txt_group_inter_lt_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = MAX_TX_TYPE_GROUP;

        txt_ctrls->txt_group_intra_lt_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = MAX_TX_TYPE_GROUP;
#if !TUNE_REMOVE_TXT_STATS
        txt_ctrls->use_stats = 0;
        txt_ctrls->intra_th = 0;
        txt_ctrls->inter_th = 0;
#endif
        break;
    case 2:
        txt_ctrls->enabled = 1;
#if TUNE_REMOVE_TXT_STATS
        txt_ctrls->txt_group_inter_lt_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = 5;

        txt_ctrls->txt_group_intra_lt_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = MAX_TX_TYPE_GROUP;
#else
        txt_ctrls->txt_group_inter_lt_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = MAX_TX_TYPE_GROUP;

        txt_ctrls->txt_group_intra_lt_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = MAX_TX_TYPE_GROUP;
#endif
#if !TUNE_REMOVE_TXT_STATS
        txt_ctrls->use_stats = 1;
        txt_ctrls->intra_th = 5;
        txt_ctrls->inter_th = 8;
#endif
        break;
    case 3:
        txt_ctrls->enabled = 1;

        txt_ctrls->txt_group_inter_lt_16x16 = 5;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = 5;

        txt_ctrls->txt_group_intra_lt_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = MAX_TX_TYPE_GROUP;
#if !TUNE_REMOVE_TXT_STATS
        txt_ctrls->use_stats = 0;
        txt_ctrls->intra_th = 0;
        txt_ctrls->inter_th = 0;
#endif
        break;
    case 4:
        txt_ctrls->enabled = 1;

        txt_ctrls->txt_group_inter_lt_16x16 = 5;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = 3;

        txt_ctrls->txt_group_intra_lt_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = MAX_TX_TYPE_GROUP;
#if !TUNE_REMOVE_TXT_STATS
        txt_ctrls->use_stats = 0;
        txt_ctrls->intra_th = 0;
        txt_ctrls->inter_th = 0;
#endif
        break;
    case 5:
        txt_ctrls->enabled = 1;

        txt_ctrls->txt_group_inter_lt_16x16 = 3;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = 2;

        txt_ctrls->txt_group_intra_lt_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = 4;
#if !TUNE_REMOVE_TXT_STATS
        txt_ctrls->use_stats = 0;
        txt_ctrls->intra_th = 0;
        txt_ctrls->inter_th = 0;
#endif
        break;
#if TUNE_M9_ME_HME_TXT
    case 6:
        txt_ctrls->enabled = 1;

        txt_ctrls->txt_group_inter_lt_16x16 = 3;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = 2;

        txt_ctrls->txt_group_intra_lt_16x16 = 3;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = 1;
#if !TUNE_REMOVE_TXT_STATS
        txt_ctrls->use_stats = 0;
        txt_ctrls->intra_th = 0;
        txt_ctrls->inter_th = 0;
#endif
        break;
#endif
    default:
        assert(0);
        break;
    }
}
#if !CLN_NSQ_AND_STATS
void set_txs_cycle_reduction_controls(ModeDecisionContext *mdctxt, uint8_t txs_cycles_red_mode) {

    TxsCycleRControls* txs_cycle_red_ctrls = &mdctxt->txs_cycles_red_ctrls;

    switch (txs_cycles_red_mode)
    {
    case 0: // txt_cycles_reduction Off
        txs_cycle_red_ctrls->enabled = 0;
        txs_cycle_red_ctrls->intra_th = 0;
        txs_cycle_red_ctrls->inter_th = 0;
        break;
    case 1:
        txs_cycle_red_ctrls->enabled = 1;
        txs_cycle_red_ctrls->intra_th = 0;
        txs_cycle_red_ctrls->inter_th = 30;
        break;
    case 2:
        txs_cycle_red_ctrls->enabled = 1;
        txs_cycle_red_ctrls->intra_th = 15;
        txs_cycle_red_ctrls->inter_th = 50;
        break;
    case 3:
        txs_cycle_red_ctrls->enabled = 1;
        txs_cycle_red_ctrls->intra_th = 25;
        txs_cycle_red_ctrls->inter_th = 75;
        break;
    default:
        assert(0);
        break;
    }
}
#endif
#if CLN_NEAR_CTRLS
void set_near_count_ctrls(ModeDecisionContext* mdctxt, uint8_t near_count_level) {

    NearCountCtrls* near_count_ctrls = &mdctxt->near_count_ctrls;

    switch (near_count_level)
    {
    case 0:
        near_count_ctrls->enabled = 0;

        near_count_ctrls->near_count = 0;
        near_count_ctrls->near_near_count = 0;

        break;
    case 1:
        near_count_ctrls->enabled = 1;

        near_count_ctrls->near_count = 3;
        near_count_ctrls->near_near_count = 3;

        break;
#if TUNE_NEAR_CTRLS
    case 2:
        near_count_ctrls->enabled = 1;
        near_count_ctrls->near_count = 1;
        near_count_ctrls->near_near_count = 3;
        break;

    case 3:
        near_count_ctrls->enabled = 1;
        near_count_ctrls->near_count = 1;
        near_count_ctrls->near_near_count = 1;
        break;

    case 4:
        near_count_ctrls->enabled = 1;
        near_count_ctrls->near_count = 0;
        near_count_ctrls->near_near_count = 0;
        break;
#else
    case 2:
        near_count_ctrls->enabled = 1;
#if TUNE_M10_SHUT_NEAR_NEAR
        near_count_ctrls->near_count = 1;
        near_count_ctrls->near_near_count = 1;
#else
        near_count_ctrls->near_count = 0;
        near_count_ctrls->near_near_count = 3;
#endif
        break;
#endif
    default:
        assert(0);
        break;
    }
}
#endif
void set_nic_controls(ModeDecisionContext *mdctxt, uint8_t nic_scaling_level) {

    NicCtrls* nic_ctrls = &mdctxt->nic_ctrls;
#if CLN_MD_CAND_BUFF
        nic_ctrls->stage1_scaling_num = MD_STAGE_NICS_SCAL_NUM[nic_scaling_level][MD_STAGE_1];
        nic_ctrls->stage2_scaling_num = MD_STAGE_NICS_SCAL_NUM[nic_scaling_level][MD_STAGE_2];
        nic_ctrls->stage3_scaling_num = MD_STAGE_NICS_SCAL_NUM[nic_scaling_level][MD_STAGE_3];

#else
    switch (nic_scaling_level)
    {
    case 0:
        nic_ctrls->stage1_scaling_num = 20;
        nic_ctrls->stage2_scaling_num = 20;
        nic_ctrls->stage3_scaling_num = 20;
        break;
#if TUNE_MR_M0_FEATURES
    case 1:
        nic_ctrls->stage1_scaling_num = 18;
        nic_ctrls->stage2_scaling_num = 18;
        nic_ctrls->stage3_scaling_num = 18;
        break;
    case 2:
        nic_ctrls->stage1_scaling_num = 16;
        nic_ctrls->stage2_scaling_num = 16;
        nic_ctrls->stage3_scaling_num = 16;
        break;
#else
    case 1:
        nic_ctrls->stage1_scaling_num = 16;
        nic_ctrls->stage2_scaling_num = 16;
        nic_ctrls->stage3_scaling_num = 16;
        break;
    case 2:
        nic_ctrls->stage1_scaling_num = 14;
        nic_ctrls->stage2_scaling_num = 14;
        nic_ctrls->stage3_scaling_num = 14;
        break;
#endif
    case 3:
        nic_ctrls->stage1_scaling_num = 12;
        nic_ctrls->stage2_scaling_num = 12;
        nic_ctrls->stage3_scaling_num = 12;
        break;
    case 4:
        nic_ctrls->stage1_scaling_num = 10;
        nic_ctrls->stage2_scaling_num = 10;
        nic_ctrls->stage3_scaling_num = 10;
        break;
    case 5:
        nic_ctrls->stage1_scaling_num = 8;
        nic_ctrls->stage2_scaling_num = 8;
        nic_ctrls->stage3_scaling_num = 8;
        break;
    case 6:
        nic_ctrls->stage1_scaling_num = 6;
        nic_ctrls->stage2_scaling_num = 6;
        nic_ctrls->stage3_scaling_num = 6;
        break;
    case 7:
        nic_ctrls->stage1_scaling_num = 5;
        nic_ctrls->stage2_scaling_num = 5;
        nic_ctrls->stage3_scaling_num = 5;
        break;
    case 8:
        nic_ctrls->stage1_scaling_num = 4;
        nic_ctrls->stage2_scaling_num = 4;
        nic_ctrls->stage3_scaling_num = 4;
        break;
    case 9:
        nic_ctrls->stage1_scaling_num = 5;
        nic_ctrls->stage2_scaling_num = 3;
        nic_ctrls->stage3_scaling_num = 3;
        break;
    case 10:
        nic_ctrls->stage1_scaling_num = 3;
        nic_ctrls->stage2_scaling_num = 3;
        nic_ctrls->stage3_scaling_num = 3;
        break;
    case 11:
        nic_ctrls->stage1_scaling_num = 3;
        nic_ctrls->stage2_scaling_num = 2;
        nic_ctrls->stage3_scaling_num = 2;
        break;
    case 12:
#if TUNE_M4_M8
        nic_ctrls->stage1_scaling_num = 3;
        nic_ctrls->stage2_scaling_num = 1;
        nic_ctrls->stage3_scaling_num = 1;
#else
        nic_ctrls->stage1_scaling_num = 3;
        nic_ctrls->stage2_scaling_num = 0;
        nic_ctrls->stage3_scaling_num = 0;
#endif
        break;
    case 13:
        nic_ctrls->stage1_scaling_num = 2;
        nic_ctrls->stage2_scaling_num = 2;
        nic_ctrls->stage3_scaling_num = 2;
        break;
#if TUNE_PRESETS_AND_PRUNING
    case 14:
#if TUNE_M7_M9
        nic_ctrls->stage1_scaling_num = 2;
        nic_ctrls->stage2_scaling_num = 0;
        nic_ctrls->stage3_scaling_num = 0;
#else
        nic_ctrls->stage1_scaling_num = 2;
        nic_ctrls->stage2_scaling_num = 1;
        nic_ctrls->stage3_scaling_num = 1;
#endif
        break;
    case 15:
#else
    case 14:
#endif
        nic_ctrls->stage1_scaling_num = 0;
        nic_ctrls->stage2_scaling_num = 0;
        nic_ctrls->stage3_scaling_num = 0;
        break;
    default:
        assert(0);
        break;
    }
#endif
}
#if FTR_NIC_PRUNING
void set_nic_pruning_controls(ModeDecisionContext *mdctxt, uint8_t nic_pruning_level) {

    NicPruningCtrls* nic_pruning_ctrls = &mdctxt->nic_pruning_ctrls;

    switch (nic_pruning_level)
    {
    case 0:
        nic_pruning_ctrls->mds1_class_th = (uint64_t)~0;
        nic_pruning_ctrls->mds2_class_th = (uint64_t)~0;
        nic_pruning_ctrls->mds3_class_th = (uint64_t)~0;

        nic_pruning_ctrls->mds1_cand_base_th = (uint64_t)~0;
        nic_pruning_ctrls->mds2_cand_base_th = (uint64_t)~0;
        nic_pruning_ctrls->mds3_cand_base_th = (uint64_t)~0;

        break;

    case 1:
        nic_pruning_ctrls->mds1_class_th = (uint64_t)~0;

        nic_pruning_ctrls->mds2_class_th = 25;
        nic_pruning_ctrls->mds2_band_cnt = 3;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds3_class_th = 25;
        nic_pruning_ctrls->mds3_band_cnt = 3;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds1_cand_base_th = (uint64_t)~0;

        nic_pruning_ctrls->mds2_cand_base_th = 45;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_cand_sq_offset_th = (uint64_t)~0;
        nic_pruning_ctrls->mds2_cand_intra_class_offset_th = (uint64_t)~0;
#endif

        nic_pruning_ctrls->mds3_cand_base_th = 45;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_cand_sq_offset_th = (uint64_t)~0;
        nic_pruning_ctrls->mds3_cand_intra_class_offset_th = (uint64_t)~0;
#endif
        break;

    case 2:
        nic_pruning_ctrls->mds1_class_th = 300;
        nic_pruning_ctrls->mds1_band_cnt = 2;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds1_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds2_class_th = 25;
        nic_pruning_ctrls->mds2_band_cnt = 3;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds3_class_th = 15;
        nic_pruning_ctrls->mds3_band_cnt = 3;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds1_cand_base_th = 300;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds1_cand_sq_offset_th = 0;
        nic_pruning_ctrls->mds1_cand_intra_class_offset_th = 0;
#endif

        nic_pruning_ctrls->mds2_cand_base_th = 15;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_cand_sq_offset_th = (uint64_t)~0;
        nic_pruning_ctrls->mds2_cand_intra_class_offset_th = (uint64_t)~0;
#endif

        nic_pruning_ctrls->mds3_cand_base_th = 15;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_cand_sq_offset_th = (uint64_t)~0;
        nic_pruning_ctrls->mds3_cand_intra_class_offset_th = (uint64_t)~0;
#endif
        break;

    case 3:
#if TUNE_LOWER_PRESETS
#if TUNE_MR_M0_FEATURES
        nic_pruning_ctrls->mds1_class_th = (uint64_t)~0;
#else
        nic_pruning_ctrls->mds1_class_th = 300;
#endif
#else
        nic_pruning_ctrls->mds1_class_th = 200;
#endif
        nic_pruning_ctrls->mds1_band_cnt = 2;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds1_scaling_factor = 1;
#endif


#if TUNE_LOWER_PRESETS
#if TUNE_MR_M0_FEATURES
        nic_pruning_ctrls->mds2_class_th = 25;
#else
        nic_pruning_ctrls->mds2_class_th = 30;
#endif
#else
        nic_pruning_ctrls->mds2_class_th = 25;
#endif
        nic_pruning_ctrls->mds2_band_cnt = 3;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_scaling_factor = 1;
#endif

#if TUNE_MR_M0_FEATURES
        nic_pruning_ctrls->mds3_class_th = 25;
#else
        nic_pruning_ctrls->mds3_class_th = 15;
#endif
#if TUNE_LOWER_PRESETS
        nic_pruning_ctrls->mds3_band_cnt = 12;
#else
        nic_pruning_ctrls->mds3_band_cnt = 3;
#endif
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_scaling_factor = 1;
#endif

#if TUNE_LOWER_PRESETS
        nic_pruning_ctrls->mds1_cand_base_th = 300;
#else
        nic_pruning_ctrls->mds1_cand_base_th = 200;
#endif
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds1_cand_sq_offset_th = 0;
        nic_pruning_ctrls->mds1_cand_intra_class_offset_th = 0;
#endif

#if TUNE_LOWER_PRESETS
        nic_pruning_ctrls->mds2_cand_base_th = 20;
#else
        nic_pruning_ctrls->mds2_cand_base_th = 15;
#endif
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_cand_sq_offset_th = (uint64_t)~0;
        nic_pruning_ctrls->mds2_cand_intra_class_offset_th = (uint64_t)~0;
#endif

#if TUNE_LOWER_PRESETS
        nic_pruning_ctrls->mds3_cand_base_th = 20;
#else
        nic_pruning_ctrls->mds3_cand_base_th = 15;
#endif
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_cand_sq_offset_th = (uint64_t)~0;
        nic_pruning_ctrls->mds3_cand_intra_class_offset_th = (uint64_t)~0;
#endif
        break;

#if TUNE_LOWER_PRESETS
    case 4:
        nic_pruning_ctrls->mds1_class_th = 300;
        nic_pruning_ctrls->mds1_band_cnt = 6;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds1_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds2_class_th = 25;
        nic_pruning_ctrls->mds2_band_cnt = 10;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds3_class_th = 15;
        nic_pruning_ctrls->mds3_band_cnt = 16;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds1_cand_base_th = 300;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds1_cand_sq_offset_th = 0;
        nic_pruning_ctrls->mds1_cand_intra_class_offset_th = 0;
#endif

        nic_pruning_ctrls->mds2_cand_base_th = 20;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_cand_sq_offset_th = (uint64_t)~0;
        nic_pruning_ctrls->mds2_cand_intra_class_offset_th = (uint64_t)~0;
#endif

        nic_pruning_ctrls->mds3_cand_base_th = 15;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_cand_sq_offset_th = (uint64_t)~0;
        nic_pruning_ctrls->mds3_cand_intra_class_offset_th = (uint64_t)~0;
#endif
        break;

    case 5:
        nic_pruning_ctrls->mds1_class_th = 200;
        nic_pruning_ctrls->mds1_band_cnt = 16;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds1_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds2_class_th = 25;
        nic_pruning_ctrls->mds2_band_cnt = 10;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds3_class_th = 15;
        nic_pruning_ctrls->mds3_band_cnt = 16;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds1_cand_base_th = 200;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds1_cand_sq_offset_th = 0;
        nic_pruning_ctrls->mds1_cand_intra_class_offset_th = 0;
#endif

        nic_pruning_ctrls->mds2_cand_base_th = 15;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_cand_sq_offset_th = (uint64_t)~0;
        nic_pruning_ctrls->mds2_cand_intra_class_offset_th = (uint64_t)~0;
#endif

        nic_pruning_ctrls->mds3_cand_base_th = 15;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_cand_sq_offset_th = (uint64_t)~0;
        nic_pruning_ctrls->mds3_cand_intra_class_offset_th = (uint64_t)~0;
#endif
        break;

    case 6:
#else
    case 4:
#endif

        nic_pruning_ctrls->mds1_class_th = 100;
        nic_pruning_ctrls->mds1_band_cnt = 2;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds1_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds2_class_th = 25;
        nic_pruning_ctrls->mds2_band_cnt = 3;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds3_class_th = 15;
        nic_pruning_ctrls->mds3_band_cnt = 3;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds1_cand_base_th = 45;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds1_cand_sq_offset_th = 0;
        nic_pruning_ctrls->mds1_cand_intra_class_offset_th = 0;
#endif

        nic_pruning_ctrls->mds2_cand_base_th = 15;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_cand_sq_offset_th = (uint64_t)~0;
        nic_pruning_ctrls->mds2_cand_intra_class_offset_th = (uint64_t)~0;
#endif

        nic_pruning_ctrls->mds3_cand_base_th = 15;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_cand_sq_offset_th = (uint64_t)~0;
        nic_pruning_ctrls->mds3_cand_intra_class_offset_th = (uint64_t)~0;
#endif
        break;

#if TUNE_LOWER_PRESETS
    case 7:
#else
    case 5:
#endif

        nic_pruning_ctrls->mds1_class_th = 100;
        nic_pruning_ctrls->mds1_band_cnt = 2;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds1_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds2_class_th = 10;
        nic_pruning_ctrls->mds2_band_cnt = 2;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds3_class_th = 10;
        nic_pruning_ctrls->mds3_band_cnt = 2;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds1_cand_base_th = 45;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds1_cand_sq_offset_th = 0;
        nic_pruning_ctrls->mds1_cand_intra_class_offset_th = 0;
#endif

        nic_pruning_ctrls->mds2_cand_base_th = 5;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_cand_sq_offset_th = 0;
        nic_pruning_ctrls->mds2_cand_intra_class_offset_th = 0;
#endif

        nic_pruning_ctrls->mds3_cand_base_th = 5;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_cand_sq_offset_th = 0;
        nic_pruning_ctrls->mds3_cand_intra_class_offset_th = 0;
#endif
        break;
#if TUNE_M4_M8
    case 8:
        nic_pruning_ctrls->mds1_class_th = 100;
        nic_pruning_ctrls->mds1_band_cnt = 2;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds1_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds2_class_th = 10;
        nic_pruning_ctrls->mds2_band_cnt = 2;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds3_class_th = 10;
        nic_pruning_ctrls->mds3_band_cnt = 2;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_scaling_factor = 1;
#endif

        nic_pruning_ctrls->mds1_cand_base_th = 45;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds1_cand_sq_offset_th = 0;
        nic_pruning_ctrls->mds1_cand_intra_class_offset_th = 0;
#endif

        nic_pruning_ctrls->mds2_cand_base_th = 1;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds2_cand_sq_offset_th = 0;
        nic_pruning_ctrls->mds2_cand_intra_class_offset_th = 0;
#endif

        nic_pruning_ctrls->mds3_cand_base_th = 1;
#if !TUNE_NEW_PRESETS_MR_M8
        nic_pruning_ctrls->mds3_cand_sq_offset_th = 0;
        nic_pruning_ctrls->mds3_cand_intra_class_offset_th = 0;
#endif
        break;
#endif
#if TUNE_PRESETS_AND_PRUNING
    case 9:
        nic_pruning_ctrls->mds1_class_th = 100;
        nic_pruning_ctrls->mds1_band_cnt = 16;
#if OPT_M9_TXT_PRED_DEPTH_PRUNING
        nic_pruning_ctrls->mds2_class_th = 5;
#else
        nic_pruning_ctrls->mds2_class_th = 10;
#endif
        nic_pruning_ctrls->mds2_band_cnt = 10;

        nic_pruning_ctrls->mds3_class_th = 10;
        nic_pruning_ctrls->mds3_band_cnt = 2;

        nic_pruning_ctrls->mds1_cand_base_th = 50;
        nic_pruning_ctrls->mds2_cand_base_th = 5;
        nic_pruning_ctrls->mds3_cand_base_th = 1;

        break;
    case 10:
        nic_pruning_ctrls->mds1_class_th = 100;
        nic_pruning_ctrls->mds1_band_cnt = 16;
#if OPT_M9_TXT_PRED_DEPTH_PRUNING
        nic_pruning_ctrls->mds2_class_th = 2;
#else
        nic_pruning_ctrls->mds2_class_th = 5;
#endif
        nic_pruning_ctrls->mds2_band_cnt = 10;

        nic_pruning_ctrls->mds3_class_th = 10;
        nic_pruning_ctrls->mds3_band_cnt = 2;

        nic_pruning_ctrls->mds1_cand_base_th = 20;
        nic_pruning_ctrls->mds2_cand_base_th = 1;
        nic_pruning_ctrls->mds3_cand_base_th = 1;
        break;
#if TUNE_M10_NIC_PRUNING
    case 11:
        nic_pruning_ctrls->mds1_class_th = 75;
        nic_pruning_ctrls->mds1_band_cnt = 16;

        nic_pruning_ctrls->mds2_class_th = 1;
        nic_pruning_ctrls->mds2_band_cnt = 2;

        nic_pruning_ctrls->mds3_class_th = 1;
        nic_pruning_ctrls->mds3_band_cnt = 2;

        nic_pruning_ctrls->mds1_cand_base_th = 1;
        nic_pruning_ctrls->mds2_cand_base_th = 1;
        nic_pruning_ctrls->mds3_cand_base_th = 1;
        break;
#endif
#endif
    default:
        assert(0);
        break;
    }
#if TUNE_LOWER_PRESETS
#if !TUNE_NEW_PRESETS_MR_M8
    nic_pruning_ctrls->mds1_cand_sq_offset_th = 0;
    nic_pruning_ctrls->mds2_cand_sq_offset_th = 0;
    nic_pruning_ctrls->mds3_cand_sq_offset_th = 0;
#endif
#endif
#if TUNE_LOWER_PRESETS
#if !TUNE_NEW_PRESETS_MR_M8
    nic_pruning_ctrls->mds1_cand_intra_class_offset_th = 0;
    nic_pruning_ctrls->mds2_cand_intra_class_offset_th = 0;
    nic_pruning_ctrls->mds3_cand_intra_class_offset_th = 0;
#endif
#endif
}
#endif
void set_inter_intra_ctrls(ModeDecisionContext* mdctxt, uint8_t inter_intra_level) {

    InterIntraCompCtrls* ii_ctrls = &mdctxt->inter_intra_comp_ctrls;

    switch (inter_intra_level) {
    case 0:
        ii_ctrls->enabled = 0;
        break;
    case 1:
        ii_ctrls->enabled = 1;
#if !FTR_NEW_REF_PRUNING_CTRLS // intra-inter
        ii_ctrls->skip_pme_unipred = 0;
        ii_ctrls->closest_ref_only = 0;
#endif
        break;
#if !FTR_NEW_REF_PRUNING_CTRLS // intra-inter
    case 2:
        ii_ctrls->enabled = 1;
        ii_ctrls->skip_pme_unipred = 1;
        ii_ctrls->closest_ref_only = 0;
        break;
    case 3:
        ii_ctrls->enabled = 1;
        ii_ctrls->skip_pme_unipred = 1;
        ii_ctrls->closest_ref_only = 1;
#endif
        break;
    default:
        assert(0);
        break;
    }
}
#if CLN_MOVE_DEPTH_REFINE_SIGS
void set_depth_ctrls(ModeDecisionContext* ctx, uint8_t depth_level) {
    DepthCtrls* depth_ctrls = &ctx->depth_ctrls;

    switch (depth_level) {
    case 0:
        depth_ctrls->s_depth = 0;
        depth_ctrls->e_depth = 0;
        break;
    case 1:
        depth_ctrls->s_depth = -2;
        depth_ctrls->e_depth = 2;
        break;
    case 2:
        depth_ctrls->s_depth = -1;
        depth_ctrls->e_depth = 1;
        break;
    default:
        assert(0);
        break;
    }
}
#endif
#if FTR_EARLY_DEPTH_REMOVAL
/*
 * Generate per-SB MD settings (do not change per-PD)
 */
EbErrorType signal_derivation_enc_dec_kernel_common(
    SequenceControlSet *scs_ptr,
    PictureControlSet *pcs_ptr,
    ModeDecisionContext *ctx) {

    EbErrorType return_error = EB_ErrorNone;

    EbEncMode enc_mode = pcs_ptr->enc_mode;
#if CLN_MOVE_DEPTH_REFINE_SIGS
    // Level 0: pred depth only
    // Level 1: [-2, +2] depth refinement
    // Level 2: [-1, +1] depth refinement
    uint8_t depth_level = 0;
    if (enc_mode <= ENC_MRS)
        depth_level = 1;
    else if (pcs_ptr->parent_pcs_ptr->sc_class1) {
        if (enc_mode <= ENC_M2)
            depth_level = pcs_ptr->slice_type == I_SLICE ? 1 : 2;
        else
            depth_level = 2;
    }
    else if(enc_mode <= ENC_M2)
        depth_level = pcs_ptr->slice_type == I_SLICE ? 1 : 2;
    else if (enc_mode <= ENC_M9)
        depth_level = 2;
    else
        depth_level = 0;

    set_depth_ctrls(ctx, depth_level);
#endif

    ctx->depth_removal_ctrls.disallow_below_64x64 = 0;
    ctx->depth_removal_ctrls.disallow_below_32x32 = 0;
    ctx->depth_removal_ctrls.disallow_below_16x16 = 0;

    // me_distortion/variance generated for 64x64 blocks only
    if (pcs_ptr->slice_type != I_SLICE && scs_ptr->static_config.super_block_size == 64) {
        // Set depth_removal_level_controls
        uint8_t depth_removal_level;
#if TUNE_SC_SETTINGS
#if FTR_ALIGN_SC_DETECOR
        if (pcs_ptr->parent_pcs_ptr->sc_class1)
#else
        if (pcs_ptr->parent_pcs_ptr->sc_content_detected)
#endif
            depth_removal_level = 0;
        else
#endif
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_SHIFT_PRESETS_DOWN
#if TUNE_FINAL_M4_M8
            if (enc_mode <= ENC_M4)
#else
            if (enc_mode <= ENC_M5)
#endif
#else
            if (enc_mode <= ENC_M6)
#endif
#else
            if (enc_mode <= ENC_M7)
#endif
            depth_removal_level = 0;
        else {
#if TUNE_DEPTH_REMOVAL_PER_RESOLUTION
#if TUNE_SHIFT_PRESETS_DOWN
                if (enc_mode <= ENC_M7) {
#else
            if (enc_mode <= ENC_M8) {
#endif
                if (scs_ptr->input_resolution < INPUT_SIZE_480p_RANGE)
                    depth_removal_level = 1;
                else if (scs_ptr->input_resolution < INPUT_SIZE_720p_RANGE)
                    depth_removal_level = 2;
                else if (scs_ptr->input_resolution < INPUT_SIZE_1080p_RANGE)
#if NEW_PRESETS
                    depth_removal_level = 2;
#else
                    depth_removal_level = 3;
#endif
                else
#if TUNE_M0_M8_MEGA_FEB
                    depth_removal_level = 2;
#else
                    depth_removal_level = 4;
#endif
            }
#if FIX_DEPTH_REMOVAL_SETTINGS
#if TUNE_SHIFT_PRESETS_DOWN
            else if (enc_mode <= ENC_M8) {
#else
            else if (enc_mode <= ENC_M9) {
#endif
#else
            else {
#endif
#if TUNE_M9_OPT_DEPTH_REMOVAL
#if TUNE_DEPTH_REMOVAL_M9
                if (scs_ptr->input_resolution < INPUT_SIZE_360p_RANGE)
                    depth_removal_level = (pcs_ptr->temporal_layer_index == 0) ? 2 : 3;
                else if (scs_ptr->input_resolution < INPUT_SIZE_480p_RANGE)
                    depth_removal_level = (pcs_ptr->temporal_layer_index == 0) ? 3 : 4;
                else if (scs_ptr->input_resolution < INPUT_SIZE_720p_RANGE)
                    depth_removal_level = (pcs_ptr->temporal_layer_index == 0) ? 6 : 7;
#if TUNE_FINAL_M4_M8
                else if (scs_ptr->input_resolution < INPUT_SIZE_1080p_RANGE)
                    depth_removal_level = (pcs_ptr->temporal_layer_index == 0) ? 6 : 7;
                else
                    depth_removal_level = (pcs_ptr->temporal_layer_index == 0) ? 7 : 8;
#else
                else if (scs_ptr->input_resolution < INPUT_SIZE_1080p_RANGE)
                    depth_removal_level = (pcs_ptr->temporal_layer_index == 0) ? 7 : 8;
                else
                    depth_removal_level = (pcs_ptr->temporal_layer_index == 0) ? 9 : 10;
#endif
#else
                if (scs_ptr->input_resolution < INPUT_SIZE_480p_RANGE)
                    depth_removal_level = 6;
                else if (scs_ptr->input_resolution < INPUT_SIZE_720p_RANGE)
                    depth_removal_level = 7;
                else if (scs_ptr->input_resolution < INPUT_SIZE_1080p_RANGE)
                    depth_removal_level = 8;
                else
                    depth_removal_level = 9;
#endif
#else
                if (scs_ptr->input_resolution < INPUT_SIZE_480p_RANGE)
                    depth_removal_level = 5;
                else if (scs_ptr->input_resolution < INPUT_SIZE_720p_RANGE)
                    depth_removal_level = 6;
                else if (scs_ptr->input_resolution < INPUT_SIZE_1080p_RANGE)
                    depth_removal_level = 7;
                else
                    depth_removal_level = 8;
#endif
            }
#if FIX_DEPTH_REMOVAL_SETTINGS
            else {
                if (scs_ptr->input_resolution < INPUT_SIZE_360p_RANGE)
                    depth_removal_level = (pcs_ptr->temporal_layer_index == 0) ? 3 : 4;
                else if (scs_ptr->input_resolution < INPUT_SIZE_480p_RANGE)
                    depth_removal_level = (pcs_ptr->temporal_layer_index == 0) ? 4 : 6;
                else if (scs_ptr->input_resolution < INPUT_SIZE_720p_RANGE)
                    depth_removal_level = (pcs_ptr->temporal_layer_index == 0) ? 7 : 8;
                else if (scs_ptr->input_resolution < INPUT_SIZE_1080p_RANGE)
                    depth_removal_level = (pcs_ptr->temporal_layer_index == 0) ? 9 : 10;
                else
                    depth_removal_level = 11;
            }
#endif
#else
            depth_removal_level = (scs_ptr->input_resolution <= INPUT_SIZE_480p_RANGE) ? 1 : 2;
#endif
        }

        set_depth_removal_level_controls(pcs_ptr, ctx, depth_removal_level);
    }

    // Set block_based_depth_refinement_level
    uint8_t block_based_depth_refinement_level;

#if TUNE_LOWER_PRESETS
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_SC_SETTINGS
    // do not use feature for SC
#if FTR_ALIGN_SC_DETECOR
    if (pcs_ptr->parent_pcs_ptr->sc_class1)
#else
    if (pcs_ptr->parent_pcs_ptr->sc_content_detected)
#endif
        block_based_depth_refinement_level = 0;
    else
#endif
#if TUNE_SHIFT_PRESETS_DOWN
    if (enc_mode <= ENC_M2)
#else
    if (enc_mode <= ENC_M3)
#endif
#else
    if (enc_mode <= ENC_M4)
#endif
#else
    if (enc_mode <= ENC_M5)
#endif
        block_based_depth_refinement_level = 0;
#if TUNE_M4_M8
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_M5_FEATURES
#if TUNE_M4_FEATURES
#if TUNE_SHIFT_PRESETS_DOWN && !TUNE_M0_M8_MEGA_FEB
    else if (enc_mode <= ENC_M3)
#else
    else if (enc_mode <= ENC_M4)
#endif
        block_based_depth_refinement_level = (pcs_ptr->temporal_layer_index == 0) ? 0 : 2;
#endif
#if TUNE_SHIFT_PRESETS_DOWN
#if NEW_PRESETS
#if TUNE_M0_M8_MEGA_FEB
    else if (enc_mode <= ENC_M6)
#else
    else if (enc_mode <= ENC_M5)
#endif
#else
    else if (enc_mode <= ENC_M4)
#endif
#else
    else if (enc_mode <= ENC_M5)
#endif
#else
    else if (enc_mode <= ENC_M4)
#endif
#if TUNE_M4_M5_DEC2
        block_based_depth_refinement_level = (pcs_ptr->temporal_layer_index == 0) ? 1 : 2;
#else
        block_based_depth_refinement_level = 2;
#endif
#if TUNE_M7_M9
#if TUNE_SHIFT_PRESETS_DOWN
    else if (enc_mode <= ENC_M7)
#else
    else if (enc_mode <= ENC_M8)
#endif
#else
    else if (enc_mode <= ENC_M7)
#endif
        block_based_depth_refinement_level = 3;
#if TUNE_M9_LEVELS
#if !TUNE_M7_M9
    else if (enc_mode <= ENC_M8)
        block_based_depth_refinement_level = (pcs_ptr->slice_type == I_SLICE) ? 4 : 6;
#endif
    else
#if TUNE_M7_M9
#if TUNE_DEPTH_SKIP
        block_based_depth_refinement_level = (pcs_ptr->temporal_layer_index == 0) ? 5 : 7;
#else
        block_based_depth_refinement_level = (pcs_ptr->temporal_layer_index == 0) ? 4 : 6;
#endif
#else
        block_based_depth_refinement_level = 8;
#endif
#else
    else
        block_based_depth_refinement_level = (pcs_ptr->slice_type == I_SLICE) ? 4 : 6;
#endif
#else
    else if (enc_mode <= ENC_M7)
        block_based_depth_refinement_level = 2;
    else
        block_based_depth_refinement_level = (pcs_ptr->slice_type == I_SLICE) ? 4 : 5;
#endif
#else
    else if (enc_mode <= ENC_M6)
        block_based_depth_refinement_level = 2;

    else if (enc_mode <= ENC_M7) {
        if (pcs_ptr->slice_type == I_SLICE) {
            block_based_depth_refinement_level = 4;
        }
        else {
            block_based_depth_refinement_level = 5;
        }
    }
    else {
        if (pcs_ptr->slice_type == I_SLICE) {
            block_based_depth_refinement_level = 4;
        }
        else {
            block_based_depth_refinement_level = 6;
        }
    }
#endif

    set_block_based_depth_refinement_controls(ctx, block_based_depth_refinement_level);

    return return_error;
}
/*
 * Generate per-SB/per-PD MD settings
 */
#else
/******************************************************
* Derive EncDec Settings for OQ
Input   : encoder mode and pd pass
Output  : EncDec Kernel signal(s)
******************************************************/
#endif
#if CLN_MD_CAND_BUFF
/*
* return the nic scalling level
  Used by nics control and memory allocation
*/
uint8_t  get_nic_scaling_level(PdPass pd_pass, EbEncMode enc_mode ,uint8_t temporal_layer_index ) {
    uint8_t  nic_scaling_level  = 1 ;
    if (pd_pass == PD_PASS_0)
#if TUNE_PRESETS_AND_PRUNING
        nic_scaling_level = 15;
#else
        nic_scaling_level = 14;
#endif
    else if (pd_pass == PD_PASS_1)
        nic_scaling_level = 12;
    else
        if (enc_mode <= ENC_MR)
            nic_scaling_level = 0;
#if TUNE_MR_M0_FEATURES
#if !TUNE_M0_REPOSITION
        else if (enc_mode <= ENC_M0)
#if TUNE_M0_M3_BASE_NBASE
            nic_scaling_level = (temporal_layer_index == 0) ? 0 : 1;
#else
            nic_scaling_level = 1;
#endif
#endif
#endif
#if TUNE_NEW_PRESETS_MR_M8
        else if (enc_mode <= ENC_M1)
#else
        else if (enc_mode <= ENC_M0)
#endif
#if TUNE_MR_M0_FEATURES
#if TUNE_M0_M3_BASE_NBASE
            nic_scaling_level = (temporal_layer_index == 0) ? 1 : 2;
#if !TUNE_SHIFT_M2_M1
        else if (enc_mode <= ENC_M2)
            nic_scaling_level = (temporal_layer_index == 0) ? 2 : 6;
#endif
#else
            nic_scaling_level = 2;
#endif
#else
            nic_scaling_level = 1;
#endif
#if TUNE_LOWER_PRESETS
#if !TUNE_NEW_PRESETS_MR_M8
        else if (enc_mode <= ENC_M2)
            nic_scaling_level = 2;
#endif
#if TUNE_M5_FEATURES
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M3)
#else
        else if (enc_mode <= ENC_M4)
#endif
            nic_scaling_level = 6;
#endif
#if TUNE_M4_M8
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M4)
#else
        else if (enc_mode <= ENC_M5)
#endif
#else
        else if (enc_mode <= ENC_M3)
#endif
#if TUNE_M5_FEATURES
            nic_scaling_level = 8;
#else
            nic_scaling_level = 6;
#endif
#if !TUNE_NEW_PRESETS_MR_M8
        else if (enc_mode <= ENC_M5)
            nic_scaling_level = 7;
#endif
#else
        else if (enc_mode <= ENC_M5)
            nic_scaling_level = 6;
#endif
#else
        else if (enc_mode <= ENC_M1)
            nic_scaling_level = 4;
        else if (enc_mode <= ENC_M2)
            nic_scaling_level = 5;
        else if (enc_mode <= ENC_M3)
            nic_scaling_level = 7;
#endif
#if TUNE_PRESETS_AND_PRUNING
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M5)
#else
        else if (enc_mode <= ENC_M6)
#endif
            nic_scaling_level = 10;
#endif
#if !TUNE_FINAL_M4_M8
#if FTR_NIC_PRUNING
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M6)
#else
        else if (enc_mode <= ENC_M7)
#endif
#else
        else if (enc_mode <= ENC_M6)
#endif
            nic_scaling_level = 11;
#endif
#if TUNE_PRESETS_AND_PRUNING
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M7)
#else
        else if (enc_mode <= ENC_M8)
#endif
#else
        else
#endif
            nic_scaling_level = 12;
#if TUNE_PRESETS_AND_PRUNING
#if FTR_M10
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M8)
#else
        else if (enc_mode <= ENC_M9)
#endif
#else
        else
#endif
#if OPT_M9_TXT_PRED_DEPTH_PRUNING
#if TUNE_M7_M9
            nic_scaling_level = 14;
#if FTR_M10
        else
            nic_scaling_level = 15;
#endif
#else
            nic_scaling_level = 15;
#endif
#else
            nic_scaling_level = 14;
#endif
#endif
   return nic_scaling_level ;
}

#endif

#if OPT_INIT
void set_dist_based_ref_pruning_controls(
    ModeDecisionContext *mdctxt, uint8_t dist_based_ref_pruning_level) {
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

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]            = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]           = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP]    = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]           = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]             = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]             = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]      = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]              = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]              = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]             = 1;

        break;
    case 2:
        ref_pruning_ctrls->enabled = 1;


        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP] = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP] = 0;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP] = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP] = 90;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP] = 90;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP] = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP] = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP] = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP] = 60;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST] = 60;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF] = 60;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE] = 60;
        ref_pruning_ctrls->ref_idx_2_offset = 10;
        ref_pruning_ctrls->ref_idx_3_offset = 20;


        ref_pruning_ctrls->closest_refs[PA_ME_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST] = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF] = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE] = 1;

        break;
    case 3:
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

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]            = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]           = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP]    = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]           = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]             = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]             = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]      = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]              = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]              = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]             = 1;

        break;
    case 4:
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

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]            = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]           = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP]    = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]           = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]             = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]             = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]      = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]              = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]              = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]             = 1;

        break;
    case 5:
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

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]            = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]           = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP]    = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]           = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]             = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]             = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]      = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]              = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]              = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]             = 1;
        break;
    case 6:
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

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]            = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]           = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP]    = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]           = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]             = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]             = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]      = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]              = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]              = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]             = 1;
        break;

    default: assert(0); break;
    }
}
#endif
     // Set disallow_4x4
#if CLN_FA
/*
* return the 4x4 level
  Used by signal_derivation_enc_dec_kernel_oq and memory allocation
*/
uint8_t get_disallow_4x4(EbEncMode enc_mode, EB_SLICE slice_type) {
    uint8_t disallow_4x4;
    if (enc_mode <= ENC_M0)
        disallow_4x4 = EB_FALSE;
#if TUNE_M9_FEATURES
#if TUNE_M7_M9
#if TUNE_SHIFT_PRESETS_DOWN
#if NEW_PRESETS && !TUNE_FINAL_M4_M8
    else if (enc_mode <= ENC_M7)
#else
    else if (enc_mode <= ENC_M6)
#endif
#else
    else if (enc_mode <= ENC_M7)
#endif
#else
    else if (enc_mode <= ENC_M8)
#endif
        disallow_4x4 = (slice_type == I_SLICE) ? EB_FALSE : EB_TRUE;
    else
        disallow_4x4 = EB_TRUE;
#else
    else
        disallow_4x4 = (slice_type == I_SLICE) ? EB_FALSE : EB_TRUE;
#endif
    return disallow_4x4;
}
#endif
EbErrorType signal_derivation_enc_dec_kernel_oq(
    SequenceControlSet *sequence_control_set_ptr,
    PictureControlSet *pcs_ptr,
    ModeDecisionContext *context_ptr) {
    EbErrorType return_error = EB_ErrorNone;
    EbEncMode enc_mode = pcs_ptr->enc_mode;
    uint8_t pd_pass = context_ptr->pd_pass;
#if TUNE_NEW_PRESETS && FTR_M9_AGRESSIVE_EARLY_CAN_ELIMINATION
    SequenceControlSet *scs_ptr           = (SequenceControlSet *)pcs_ptr->scs_wrapper_ptr->object_ptr;
#endif
#if !FTR_NEW_CYCLES_ALLOC
    // sb_classifier levels
    // Level                Settings
    // 0                    Off
    // 1                    TH 80%
    // 2                    TH 70%
    // 3                    TH 60%
    // 4                    TH 50%
    // 5                    TH 40%
    if (pd_pass == PD_PASS_0)
        context_ptr->enable_area_based_cycles_allocation = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->enable_area_based_cycles_allocation = 0;
    else {
        if (pcs_ptr->slice_type == I_SLICE)
            context_ptr->enable_area_based_cycles_allocation = 0;
        // Do not use cycles reduction algorithms in 480p and below
        else if (pcs_ptr->parent_pcs_ptr->input_resolution <= INPUT_SIZE_480p_RANGE)
            context_ptr->enable_area_based_cycles_allocation = 0;
        else
            context_ptr->enable_area_based_cycles_allocation = 1;
    }
#endif
    uint8_t txt_level = 0;
    if (pd_pass == PD_PASS_0)
        txt_level = 0;
    else if (pd_pass == PD_PASS_1)
        txt_level = 0;
    else
#if TUNE_M4_M8
#if TUNE_SHIFT_PRESETS_DOWN
        if (enc_mode <= ENC_M2)
#else
        if (enc_mode <= ENC_M3)
#endif
#else
#if TUNE_LOWER_PRESETS
        if (enc_mode <= ENC_M5)
#else
        if (enc_mode <= ENC_M4)
#endif
#endif
            txt_level = 1;
#if TUNE_M4_BASE_NBASE
#if TUNE_SHIFT_PRESETS_DOWN
#if NEW_PRESETS
        else if (enc_mode <= ENC_M4)
#else
        else if (enc_mode <= ENC_M3)
#endif
#else
        else if (enc_mode <= ENC_M4)
#endif
            txt_level = (pcs_ptr->temporal_layer_index == 0) ? 1 : 3;
#endif
#if TUNE_M4_M8
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M5)
#else
        else if (enc_mode <= ENC_M6)
#endif
#else
        else if (enc_mode <= ENC_M5)
#endif
            txt_level = 3;
#endif
#if TUNE_M9_ME_HME_TXT
#if TUNE_M7_M9
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M8)
#else
        else if (enc_mode <= ENC_M9)
#endif
#else
        else if (enc_mode <= ENC_M8)
#endif
            txt_level = 5;
        else
#if OPT_M9_TXT_PRED_DEPTH_PRUNING
#if FTR_M10
            txt_level = (pcs_ptr->slice_type == I_SLICE) ? 5 : 0;
#else
            txt_level = (pcs_ptr->parent_pcs_ptr->input_resolution <= INPUT_SIZE_480p_RANGE) ? 6
            : 0;

#endif
#else
            txt_level = 6;
#endif
#else
        else
            txt_level = 5;
#endif
    set_txt_controls(context_ptr, txt_level);
    if (pd_pass == PD_PASS_0)
        context_ptr->interpolation_search_level = IFS_OFF;
    else if (pd_pass == PD_PASS_1)
        context_ptr->interpolation_search_level = IFS_OFF;
    else
#if TUNE_PRESETS_CLEANUP
#if TUNE_NEW_PRESETS_MR_M8
        if (enc_mode <= ENC_MR)
#else
        if (enc_mode <= ENC_M0)
#endif
#else
        if (enc_mode <= ENC_M2)
#endif
            context_ptr->interpolation_search_level = IFS_MDS1;
#if TUNE_M9_IFS_SSE_ADAPT_ME_MV_NEAR_WM_TF
#if TUNE_M7_M9
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M8)
#else
        else if (enc_mode <= ENC_M9)
#endif
#else
        else if (enc_mode <= ENC_M8)
#endif
            context_ptr->interpolation_search_level = IFS_MDS3;
        else
            context_ptr->interpolation_search_level = (sequence_control_set_ptr->input_resolution <= INPUT_SIZE_720p_RANGE) ? IFS_MDS3 : IFS_OFF;
#else
        else
            context_ptr->interpolation_search_level = IFS_MDS3;
#endif
    // Set Chroma Mode
    // Level                Settings
    // CHROMA_MODE_0  0     Full chroma search @ MD
    // CHROMA_MODE_1  1     Fast chroma search @ MD
    // CHROMA_MODE_2  2     Chroma blind @ MD + CFL @ EP
    // CHROMA_MODE_3  3     Chroma blind @ MD + no CFL @ EP
    if (pd_pass == PD_PASS_0)
        context_ptr->chroma_level = CHROMA_MODE_2; // or CHROMA_MODE_3
    else if (pd_pass == PD_PASS_1) {
        context_ptr->chroma_level = CHROMA_MODE_1;
    }
    else if (sequence_control_set_ptr->static_config.set_chroma_mode ==
        DEFAULT) {
#if TUNE_M4_M8 && !TUNE_SHIFT_PRESETS_DOWN
        if (enc_mode <= ENC_M6)
#else
        if (enc_mode <= ENC_M5)
#endif
            context_ptr->chroma_level = CHROMA_MODE_0;
        else
            context_ptr->chroma_level = CHROMA_MODE_1;
    }
    else // use specified level
        context_ptr->chroma_level =
        sequence_control_set_ptr->static_config.set_chroma_mode;

    // Chroma independent modes search
    // Level                Settings
    // 0                    post first md_stage
    // 1                    post last md_stage
#if TUNE_PRESETS_CLEANUP
    if (enc_mode <= ENC_MRS) {
#else
    if (enc_mode <= ENC_MR) {
#endif
        context_ptr->chroma_at_last_md_stage = 0;
        context_ptr->chroma_at_last_md_stage_intra_th = (uint64_t)~0;
        context_ptr->chroma_at_last_md_stage_cfl_th = (uint64_t)~0;
    }
#if TUNE_LOWER_PRESETS
    else if (enc_mode <= ENC_M1) {
#else
    else if (enc_mode <= ENC_M2) {
#endif
        context_ptr->chroma_at_last_md_stage = (context_ptr->chroma_level == CHROMA_MODE_0) ? 1 : 0;
        context_ptr->chroma_at_last_md_stage_intra_th = 130;
        context_ptr->chroma_at_last_md_stage_cfl_th = 130;
    }
#if TUNE_M0_M3_BASE_NBASE
#if !TUNE_SHIFT_M2_M1
    else if (enc_mode <= ENC_M2) {
        if (pcs_ptr->temporal_layer_index == 0) {
            context_ptr->chroma_at_last_md_stage = (context_ptr->chroma_level == CHROMA_MODE_0) ? 1 : 0;
            context_ptr->chroma_at_last_md_stage_intra_th = 130;
            context_ptr->chroma_at_last_md_stage_cfl_th = 130;
        }
        else {
            context_ptr->chroma_at_last_md_stage = (context_ptr->chroma_level == CHROMA_MODE_0) ? 1 : 0;
            context_ptr->chroma_at_last_md_stage_intra_th = 100;
            context_ptr->chroma_at_last_md_stage_cfl_th = 100;
        }
    }
#endif
#endif
    else {
        context_ptr->chroma_at_last_md_stage = (context_ptr->chroma_level == CHROMA_MODE_0) ? 1 : 0;
        context_ptr->chroma_at_last_md_stage_intra_th = 100;
        context_ptr->chroma_at_last_md_stage_cfl_th = 100;
    }
    // Cfl level
    // Level                Settings
    // 0                    Allow cfl
    // 1                    Disable cfl
#if TUNE_M4_M8
#if TUNE_CFL_LEVEL_M8_M9
#if TUNE_M7_M9
#if TUNE_SHIFT_PRESETS_DOWN
    if (enc_mode <= ENC_M5)
#else
    if (enc_mode <= ENC_M6)
#endif
#else
    if (enc_mode <= ENC_M7)
#endif
        context_ptr->md_disable_cfl = EB_FALSE;
    else
        context_ptr->md_disable_cfl = (pcs_ptr->temporal_layer_index == 0) ? EB_FALSE : EB_TRUE;
#else
    context_ptr->md_disable_cfl = EB_FALSE;
#endif
#else
    if (enc_mode <= ENC_M6)
        context_ptr->md_disable_cfl = EB_FALSE;
    else
        context_ptr->md_disable_cfl = pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? EB_FALSE : EB_TRUE;
#endif
     // Set disallow_4x4
#if CLN_FA
    context_ptr->disallow_4x4 = get_disallow_4x4 (enc_mode, pcs_ptr->slice_type);
#else
    if (enc_mode <= ENC_M0)
         context_ptr->disallow_4x4 = EB_FALSE;
#if TUNE_M9_FEATURES
#if TUNE_M7_M9
#if TUNE_SHIFT_PRESETS_DOWN
#if NEW_PRESETS
    else if (enc_mode <= ENC_M7)
#else
    else if (enc_mode <= ENC_M6)
#endif
#else
    else if (enc_mode <= ENC_M7)
#endif
#else
    else if (enc_mode <= ENC_M8)
#endif
        context_ptr->disallow_4x4 = (pcs_ptr->slice_type == I_SLICE) ? EB_FALSE : EB_TRUE;
    else
        context_ptr->disallow_4x4 = EB_TRUE;
#else
     else
         context_ptr->disallow_4x4 = (pcs_ptr->slice_type == I_SLICE) ? EB_FALSE : EB_TRUE;
#endif
#endif
     // If SB non-multiple of 4, then disallow_4x4 could not be used
     // SB Stats
     uint32_t sb_width =
         MIN(sequence_control_set_ptr->sb_size_pix, pcs_ptr->parent_pcs_ptr->aligned_width - context_ptr->sb_ptr->origin_x);
     uint32_t sb_height =
         MIN(sequence_control_set_ptr->sb_size_pix, pcs_ptr->parent_pcs_ptr->aligned_height - context_ptr->sb_ptr->origin_y);
     if (sb_width % 8 != 0 || sb_height % 8 != 0) {
         context_ptr->disallow_4x4 = EB_FALSE;
     }

     if (pd_pass == PD_PASS_0)
#if TUNE_LOWER_PRESETS
         context_ptr->md_disallow_nsq = enc_mode <= ENC_M0 ? pcs_ptr->parent_pcs_ptr->disallow_nsq : 1;
#else
         context_ptr->md_disallow_nsq = enc_mode <= ENC_MR ? pcs_ptr->parent_pcs_ptr->disallow_nsq : 1;
#endif
     else if (pd_pass == PD_PASS_1)
         context_ptr->md_disallow_nsq = enc_mode <= ENC_MR ? pcs_ptr->parent_pcs_ptr->disallow_nsq : 1;
     else
         // Update nsq settings based on the sb_class
         context_ptr->md_disallow_nsq = pcs_ptr->parent_pcs_ptr->disallow_nsq;


     if (pd_pass == PD_PASS_0)
         context_ptr->global_mv_injection = 0;
     else if (pd_pass == PD_PASS_1)
         context_ptr->global_mv_injection = 0;
     else
         context_ptr->global_mv_injection = pcs_ptr->parent_pcs_ptr->gm_ctrls.enabled;

    if (pd_pass == PD_PASS_0)
        context_ptr->new_nearest_injection = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->new_nearest_injection = 0;
    else
        context_ptr->new_nearest_injection = 1;

    if (pd_pass == PD_PASS_0)
        context_ptr->new_nearest_near_comb_injection = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->new_nearest_near_comb_injection = 0;
    else

        if (sequence_control_set_ptr->static_config.new_nearest_comb_inject ==
            DEFAULT)
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_M2_FEATURES
#if TUNE_M1_REPOSITION
            if (enc_mode <= ENC_M0)
#else
            if (enc_mode <= ENC_M1)
#endif
#else
            if (enc_mode <= ENC_M2)
#endif
#else
                if (enc_mode <= ENC_M0)
#endif
                    context_ptr->new_nearest_near_comb_injection = 1;
                else
                    context_ptr->new_nearest_near_comb_injection = 0;
        else
            context_ptr->new_nearest_near_comb_injection =
            sequence_control_set_ptr->static_config.new_nearest_comb_inject;

#if CLN_NEAR_CTRLS
    uint8_t near_count_level = 0;

    if (pd_pass == PD_PASS_0)
        near_count_level = 0;
    if (pd_pass == PD_PASS_1)
        near_count_level = 0;
    else
#if TUNE_SHIFT_PRESETS_DOWN
#if TUNE_NEAR_CTRLS && !TUNE_FINAL_M4_M8
        if (enc_mode <= ENC_M7)
#else
        if (enc_mode <= ENC_M8)
#endif
#else
        if (enc_mode <= ENC_M9)
#endif
            near_count_level = 1;
        else
            near_count_level = 2;
    set_near_count_ctrls(context_ptr, near_count_level);
#endif


    // Set warped motion injection
    // Level                Settings
    // 0                    OFF
    // 1                    On

    if (pd_pass == PD_PASS_0) {
        context_ptr->warped_motion_injection = 0;
    }
    else if (pd_pass == PD_PASS_1) {
        context_ptr->warped_motion_injection = 1;
    }
    else
        context_ptr->warped_motion_injection = 1;

    // Set unipred3x3 injection
    // Level                Settings
    // 0                    OFF
    // 1                    ON FULL
    // 2                    Reduced set
    if (pd_pass == PD_PASS_0) {
        context_ptr->unipred3x3_injection = 0;
    }
    else if (pd_pass == PD_PASS_1) {
        context_ptr->unipred3x3_injection = 2;
    }
    else
    {
#if TUNE_M1_REPOSITION
        if (enc_mode <= ENC_M0)
            context_ptr->unipred3x3_injection = 1;
        else if (enc_mode <= ENC_M1)
            context_ptr->unipred3x3_injection = 2;
#else
        if (enc_mode <= ENC_M1)
            context_ptr->unipred3x3_injection = 1;
#endif
        else
            context_ptr->unipred3x3_injection = 0;
    }

    // Set bipred3x3 injection
    // Level                Settings
    // 0                    OFF
    // 1                    ON FULL
    // 2                    Reduced set
    if (pd_pass == PD_PASS_0) {
        context_ptr->bipred3x3_injection = 0;
    }
    else if (pd_pass == PD_PASS_1) {
        context_ptr->bipred3x3_injection = 2;
    }
    else if (sequence_control_set_ptr->static_config.bipred_3x3_inject == DEFAULT) {
#if TUNE_PRESETS_CLEANUP
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_SHIFT_M2_M1
        if (enc_mode <= ENC_M1)
#else
        if (enc_mode <= ENC_M2)
#endif
#else
        if (enc_mode <= ENC_M0)
#endif
#else
        if (enc_mode <= ENC_M1)
#endif
            context_ptr->bipred3x3_injection = 1;
#if TUNE_M4_M8
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M5)
#else
        else if (enc_mode <= ENC_M6)
#endif
#else
        else if (enc_mode <= ENC_M5)
#endif
            context_ptr->bipred3x3_injection = 2;
        else
            context_ptr->bipred3x3_injection = 0;
        }
    else{
        context_ptr->bipred3x3_injection =
        sequence_control_set_ptr->static_config.bipred_3x3_inject;
        }

    context_ptr->inject_inter_candidates = 1;

    if (sequence_control_set_ptr->compound_mode) {
        if (sequence_control_set_ptr->static_config.compound_level == DEFAULT) {
            if (pd_pass == PD_PASS_0)
                context_ptr->inter_compound_mode = 0;
            else if (pd_pass == PD_PASS_1)
                context_ptr->inter_compound_mode = 0;
#if FTR_UPGRADE_COMP_LEVELS
            else if (enc_mode <= ENC_MR)
                context_ptr->inter_compound_mode = 1;
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_M2_FEATURES
#if TUNE_M0_M3_BASE_NBASE
#if !TUNE_M0_REPOSITION
            else if (enc_mode <= ENC_M0)
                context_ptr->inter_compound_mode = (pcs_ptr->temporal_layer_index == 0) ? 1 : 2;
#endif
#endif
#if TUNE_M1_REPOSITION
#if TUNE_M0_M8_MEGA_FEB
            else if (enc_mode <= ENC_M3)
#else
            else if (enc_mode <= ENC_M0)
#endif

#else
            else if (enc_mode <= ENC_M1)
#endif
#else
            else if (enc_mode <= ENC_M2)
#endif
#else
            else if (enc_mode <= ENC_M0)
#endif
                context_ptr->inter_compound_mode = 2;
#if TUNE_M0_M3_BASE_NBASE && !TUNE_M0_M8_MEGA_FEB
#if TUNE_SHIFT_M2_M1
            else if (enc_mode <= ENC_M1)
#else
            else if (enc_mode <= ENC_M2)
#endif
                context_ptr->inter_compound_mode = (pcs_ptr->temporal_layer_index == 0) ? 2 : 3;
#endif
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_SHIFT_PRESETS_DOWN
            else if (enc_mode <= ENC_M4)
#else
            else if (enc_mode <= ENC_M5)
#endif
#else
            else if (enc_mode <= ENC_M4)
#endif
                context_ptr->inter_compound_mode = 3;
#if !TUNE_NEW_PRESETS_MR_M8
            else if (enc_mode <= ENC_M5)
                context_ptr->inter_compound_mode = 4;
            else  if (enc_mode <= ENC_M6)
                context_ptr->inter_compound_mode = 5;
#endif
#if TUNE_M9_GM_INTER_COMPOUND
#if TUNE_SHIFT_PRESETS_DOWN
#if TUNE_FINAL_M4_M8
            else if (enc_mode <= ENC_M5)
#else
            else if (enc_mode <= ENC_M7)
#endif
#else
            else if (enc_mode <= ENC_M8)
#endif
                context_ptr->inter_compound_mode = (pcs_ptr->parent_pcs_ptr->input_resolution == INPUT_SIZE_240p_RANGE) ? 5 : 0;
            else
                context_ptr->inter_compound_mode = 0;
#else
            else
                context_ptr->inter_compound_mode = (pcs_ptr->parent_pcs_ptr->input_resolution == INPUT_SIZE_240p_RANGE) ? 5 : 0;
#endif
#else
            else if (enc_mode <= ENC_MR)
                context_ptr->inter_compound_mode = 1;
            else if (enc_mode <= ENC_M0)
                context_ptr->inter_compound_mode = 3;
#if TUNE_LOWER_PRESETS
            else if (enc_mode <= ENC_M2)
#else
            else if (enc_mode <= ENC_M3)
#endif
                context_ptr->inter_compound_mode = 4;
#if !TUNE_LOWER_PRESETS
            else if (enc_mode <= ENC_M4)
                context_ptr->inter_compound_mode = 6;
#endif
            else
                context_ptr->inter_compound_mode = 0;
#endif
        }
        else {
            context_ptr->inter_compound_mode = sequence_control_set_ptr->static_config.compound_level;
        }
    }
    else {
        context_ptr->inter_compound_mode = 0;
    }
#if OPT_INIT
    // Set dist_based_ref_pruning
    if (pcs_ptr->parent_pcs_ptr->ref_list0_count_try > 1 || pcs_ptr->parent_pcs_ptr->ref_list1_count_try > 1) {
        if (context_ptr->pd_pass == PD_PASS_0)
            context_ptr->dist_based_ref_pruning = 0;
        else if (context_ptr->pd_pass == PD_PASS_1)
            context_ptr->dist_based_ref_pruning = 0;
        else if (enc_mode <= ENC_MR)
            context_ptr->dist_based_ref_pruning = 1;
        else if (enc_mode <= ENC_M0)
            context_ptr->dist_based_ref_pruning = (pcs_ptr->temporal_layer_index == 0) ? 1 : 2;
        else if (enc_mode <= ENC_M7)
            context_ptr->dist_based_ref_pruning = (pcs_ptr->temporal_layer_index == 0) ? 2 : 4;
        else
            context_ptr->dist_based_ref_pruning = 4;
    }
    else {
        context_ptr->dist_based_ref_pruning = 0;
    }

    set_dist_based_ref_pruning_controls(context_ptr, context_ptr->dist_based_ref_pruning);
#endif
    if (pd_pass == PD_PASS_0) {
        context_ptr->md_staging_mode = MD_STAGING_MODE_0;
    }
    else if (pd_pass == PD_PASS_1) {
        context_ptr->md_staging_mode = MD_STAGING_MODE_1;
    }
    else
#if TUNE_LOWER_PRESETS
#if TUNE_SHIFT_PRESETS_DOWN
        if (enc_mode <= ENC_M4)
#else
        if (enc_mode <= ENC_M5)
#endif
#else
        if (enc_mode <= ENC_M3)
#endif
            context_ptr->md_staging_mode = MD_STAGING_MODE_2;
        else
            context_ptr->md_staging_mode = MD_STAGING_MODE_1;


    // spatial_sse_full_loop_level | Default Encoder Settings            | Command Line Settings
    //             0               | OFF subject to possible constraints | OFF in PD_PASS_2
    //             1               | ON subject to possible constraints  | ON in PD_PASS_2
    if (pd_pass == PD_PASS_0)
        context_ptr->spatial_sse_full_loop_level = EB_FALSE;
    else if (pd_pass == PD_PASS_1)
        context_ptr->spatial_sse_full_loop_level = EB_FALSE;
    else if (sequence_control_set_ptr->static_config.spatial_sse_full_loop_level == DEFAULT)
#if TUNE_M9_IFS_SSE_ADAPT_ME_MV_NEAR_WM_TF
#if TUNE_M7_M9
#if TUNE_SHIFT_PRESETS_DOWN
        if (enc_mode <= ENC_M8)
#else
        if (enc_mode <= ENC_M9)
#endif
#else
        if (enc_mode <= ENC_M8)
#endif
            context_ptr->spatial_sse_full_loop_level = EB_TRUE;
#else
        if (enc_mode <= ENC_M9)
            context_ptr->spatial_sse_full_loop_level = EB_TRUE;
#endif
        else
            context_ptr->spatial_sse_full_loop_level = EB_FALSE;
    else
        context_ptr->spatial_sse_full_loop_level =
        sequence_control_set_ptr->static_config.spatial_sse_full_loop_level;

    if (context_ptr->chroma_level <= CHROMA_MODE_1)
        context_ptr->blk_skip_decision = EB_TRUE;
    else
        context_ptr->blk_skip_decision = EB_FALSE;

    if (pd_pass == PD_PASS_0)
        context_ptr->rdoq_level = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->rdoq_level = 0;
    else
#if TUNE_M8_TO_MATCH_M7
#if TUNE_M9_FEATURES
#if OPT_RDOQ_FOR_M9
#if TUNE_M7_M9
#if TUNE_SHIFT_PRESETS_DOWN
        if (enc_mode <= ENC_M8)
#else
        if (enc_mode <= ENC_M9)
#endif
#else
        if (enc_mode <= ENC_M8)
#endif
#else
        if (enc_mode <= ENC_M9)
#endif
#else
        if (enc_mode <= ENC_M8)
#endif
#else
        if (enc_mode <= ENC_M7)
#endif
            context_ptr->rdoq_level = 1;
        else
            context_ptr->rdoq_level = (pcs_ptr->parent_pcs_ptr->slice_type == I_SLICE) ? 2 : 3;
    set_rdoq_controls(context_ptr, context_ptr->rdoq_level);

    // Derive redundant block
    if (pd_pass == PD_PASS_0)
        context_ptr->redundant_blk = EB_FALSE;
    else if (pd_pass == PD_PASS_1)
        context_ptr->redundant_blk = EB_TRUE;
    else
        if (sequence_control_set_ptr->static_config.enable_redundant_blk ==
            DEFAULT)
#if FTR_M10
            if (enc_mode <= ENC_M10)
#else
            if (enc_mode <= ENC_M9)
#endif
                context_ptr->redundant_blk = EB_TRUE;
            else
                context_ptr->redundant_blk = EB_FALSE;
        else
            context_ptr->redundant_blk =
            sequence_control_set_ptr->static_config.enable_redundant_blk;
#if !FTR_NIC_PRUNING
    // md_stage_1_cand_prune_th (for single candidate removal per class)
    // Remove candidate if deviation to the best is higher than md_stage_1_cand_prune_th
    if (pd_pass == PD_PASS_0)
        context_ptr->md_stage_1_cand_prune_th = (uint64_t)~0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_stage_1_cand_prune_th = 75;
    else
        if (enc_mode <= ENC_MR)
            context_ptr->md_stage_1_cand_prune_th = (uint64_t)~0;
        else if (enc_mode <= ENC_M1)
            context_ptr->md_stage_1_cand_prune_th = 300;
        else if (enc_mode <= ENC_M5)
            context_ptr->md_stage_1_cand_prune_th = 200;
        else
            context_ptr->md_stage_1_cand_prune_th = 45;

    // md_stage_1_class_prune_th (for class removal)
    // Remove class if deviation to the best higher than TH_C
    if (pd_pass == PD_PASS_0)
        context_ptr->md_stage_1_class_prune_th = (uint64_t)~0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_stage_1_class_prune_th = 100;
    else
        if (enc_mode <= ENC_MR)
            context_ptr->md_stage_1_class_prune_th = (uint64_t)~0;
        else if (enc_mode <= ENC_M1)
            context_ptr->md_stage_1_class_prune_th = 300;
        else if (enc_mode <= ENC_M5)
            context_ptr->md_stage_1_class_prune_th = 200;
        else
            context_ptr->md_stage_1_class_prune_th = 100;

   // md_stage_2_cand_prune_th (for single candidate removal per class)
   // Remove candidate if deviation to the best is higher than
   // md_stage_2_cand_prune_th
    if (pd_pass == PD_PASS_0)
        context_ptr->md_stage_2_cand_prune_th = (uint64_t)~0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_stage_2_cand_prune_th = 5;
    else
        if (enc_mode <= ENC_MRS)
            context_ptr->md_stage_2_cand_prune_th = (uint64_t)~0;
        else
            if (enc_mode <= ENC_MR)
                context_ptr->md_stage_2_cand_prune_th = 45;
            else if (enc_mode <= ENC_M9)
                context_ptr->md_stage_2_cand_prune_th = 15;
            else
                context_ptr->md_stage_2_cand_prune_th = 5;

    // md_stage_2_class_prune_th (for class removal)
    // Remove class if deviation to the best is higher than
    // md_stage_2_class_prune_th
    if (pd_pass == PD_PASS_0)
        context_ptr->md_stage_2_class_prune_th = (uint64_t)~0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_stage_2_class_prune_th = 25;
    else
        context_ptr->md_stage_2_class_prune_th = 25;

   // md_stage_3_cand_prune_th (for single candidate removal per class)
   // Remove candidate if deviation to the best is higher than
   // md_stage_3_cand_prune_th
    if (pd_pass == PD_PASS_0)
        context_ptr->md_stage_3_cand_prune_th = (uint64_t)~0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_stage_3_cand_prune_th = 5;
    else
        if (enc_mode <= ENC_MRS)
            context_ptr->md_stage_3_cand_prune_th = (uint64_t)~0;
        else
            if (enc_mode <= ENC_MR)
                context_ptr->md_stage_3_cand_prune_th = 45;
            else if (enc_mode <= ENC_M9)
                context_ptr->md_stage_3_cand_prune_th = 15;
            else
                context_ptr->md_stage_3_cand_prune_th = 5;

    // md_stage_3_class_prune_th (for class removal)
    // Remove class if deviation to the best is higher than
    // md_stage_3_class_prune_th
    if (pd_pass == PD_PASS_0)
        context_ptr->md_stage_3_class_prune_th = (uint64_t)~0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_stage_3_class_prune_th = 25;
    else
        context_ptr->md_stage_3_class_prune_th = 25;
#endif
#if FTR_NEW_CYCLES_ALLOC
    uint8_t parent_sq_coeff_area_based_cycles_reduction_level = 0;
    if (pd_pass == PD_PASS_0)
        parent_sq_coeff_area_based_cycles_reduction_level = 0;
    else if (pd_pass == PD_PASS_1)
        parent_sq_coeff_area_based_cycles_reduction_level = 0;
#if TUNE_M4_M8
    else if (enc_mode <= ENC_MRS)
        parent_sq_coeff_area_based_cycles_reduction_level = 0;
    else if (enc_mode <= ENC_MR)
        parent_sq_coeff_area_based_cycles_reduction_level = pcs_ptr->slice_type == I_SLICE ? 0 : 1;
#if TUNE_M0_M3_BASE_NBASE
#if !TUNE_M0_REPOSITION
    else if (enc_mode <= ENC_M0)
        parent_sq_coeff_area_based_cycles_reduction_level = pcs_ptr->slice_type == I_SLICE ? 0 : (pcs_ptr->temporal_layer_index == 0) ? 1 : 2;
#endif
#endif
    else if (enc_mode <= ENC_M1)
        parent_sq_coeff_area_based_cycles_reduction_level = pcs_ptr->slice_type == I_SLICE ? 0 : 2;
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_M3_REPOSITION
#if TUNE_SHIFT_PRESETS_DOWN
    else if (enc_mode <= ENC_M2)
#else
    else if (enc_mode <= ENC_M3)
#endif
#else
    else if (enc_mode <= ENC_M2)
#endif
#if TUNE_M0_M3_BASE_NBASE
        parent_sq_coeff_area_based_cycles_reduction_level = pcs_ptr->slice_type == I_SLICE ? 0
        : (pcs_ptr->temporal_layer_index == 0)? 2 : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 4 : 7;
#else
        parent_sq_coeff_area_based_cycles_reduction_level = pcs_ptr->slice_type == I_SLICE ? 0 : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 4 : 7;
#endif
#if !NEW_PRESETS
#if TUNE_PRESETS_AND_PRUNING && !TUNE_M4_M5_DEC2
    else if (enc_mode <= ENC_M4)
#else
#if TUNE_M4_REPOSITION
#if TUNE_SHIFT_PRESETS_DOWN
    else if (enc_mode <= ENC_M3)
#else
    else if (enc_mode <= ENC_M4)
#endif
#else
    else if (enc_mode <= ENC_M3)
#endif
#endif
#if TUNE_M0_M3_BASE_NBASE
        parent_sq_coeff_area_based_cycles_reduction_level = pcs_ptr->slice_type == I_SLICE ? (pcs_ptr->temporal_layer_index == 0) ? 0 : 5
        : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 4 : 7;
#else
        parent_sq_coeff_area_based_cycles_reduction_level = pcs_ptr->slice_type == I_SLICE ? 5 : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 4 : 7;
#endif
#endif
#if TUNE_M4_BASE_NBASE && !TUNE_M4_REPOSITION
    else if (enc_mode <= ENC_M4)
        parent_sq_coeff_area_based_cycles_reduction_level = pcs_ptr->slice_type == I_SLICE ? 5
        : (pcs_ptr->temporal_layer_index == 0) ? pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 4 : 7
        : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 6 : 7;
#endif
    else
        parent_sq_coeff_area_based_cycles_reduction_level = pcs_ptr->slice_type == I_SLICE ? 5 : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 6 : 7;
#else
    else if (enc_mode <= ENC_M2)
        parent_sq_coeff_area_based_cycles_reduction_level = pcs_ptr->slice_type == I_SLICE ? 0 : 3;
    else if (enc_mode <= ENC_M3)
        parent_sq_coeff_area_based_cycles_reduction_level = pcs_ptr->slice_type == I_SLICE ? 0 : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 4 : 7;
    else
        parent_sq_coeff_area_based_cycles_reduction_level = pcs_ptr->slice_type == I_SLICE ? 5 : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 6 : 7;
#endif
#else
    else if (pcs_ptr->slice_type == I_SLICE)
        parent_sq_coeff_area_based_cycles_reduction_level = 0;
    else if (enc_mode <= ENC_MRS)
        parent_sq_coeff_area_based_cycles_reduction_level = 0;
    else if (enc_mode <= ENC_MR)
        parent_sq_coeff_area_based_cycles_reduction_level = 1;
    else if (enc_mode <= ENC_M1)
        parent_sq_coeff_area_based_cycles_reduction_level = 2;
    else if (enc_mode <= ENC_M2)
#if TUNE_LOWER_PRESETS
        parent_sq_coeff_area_based_cycles_reduction_level = 3;
    else
        parent_sq_coeff_area_based_cycles_reduction_level = pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 4 : 5;
#else
        parent_sq_coeff_area_based_cycles_reduction_level = pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 3 : 4;
    else
        parent_sq_coeff_area_based_cycles_reduction_level = 4;
#endif
#endif
    set_parent_sq_coeff_area_based_cycles_reduction_ctrls(context_ptr, pcs_ptr->parent_pcs_ptr->input_resolution, parent_sq_coeff_area_based_cycles_reduction_level);
#else
    // If using a mode offset, do not modify the NSQ-targeting features
    if (pd_pass == PD_PASS_0)
        context_ptr->coeff_area_based_bypass_nsq_th = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->coeff_area_based_bypass_nsq_th = 0;
    else
        context_ptr->coeff_area_based_bypass_nsq_th = context_ptr->enable_area_based_cycles_allocation ? nsq_cycles_reduction_th[context_ptr->sb_class] : 0;
#endif
#if !CLN_NSQ_AND_STATS
    adaptive_md_cycles_redcution_controls(context_ptr, 0);
#endif
        // Weighting (expressed as a percentage) applied to
        // square shape costs for determining if a and b
        // shapes should be skipped. Namely:
        // skip HA, HB, and H4 if h_cost > (weighted sq_cost)
        // skip VA, VB, and V4 if v_cost > (weighted sq_cost)
        if (pd_pass == PD_PASS_0)
            context_ptr->sq_weight = (uint32_t)~0;
        else if (pd_pass == PD_PASS_1)
            context_ptr->sq_weight = 100;

        else
            if (enc_mode <= ENC_MRS)
                context_ptr->sq_weight = (uint32_t)~0;
            else
#if TUNE_M0_M8_MEGA_FEB
                if (enc_mode <= ENC_M0)
#else
                if (enc_mode <= ENC_MR)
#endif
#if TUNE_PRESETS_CLEANUP
                    context_ptr->sq_weight = 105;
#else
                    context_ptr->sq_weight = 115;
#endif
#if !TUNE_M0_M8_MEGA_FEB
                else
                    if (enc_mode <= ENC_M0)
#if TUNE_PRESETS_CLEANUP
#if TUNE_MR_M0_FEATURES
                        context_ptr->sq_weight = 102;
#else
                        context_ptr->sq_weight = 100;
#endif
#else
                        context_ptr->sq_weight = 105;
#endif
#endif
#if TUNE_LOWER_PRESETS
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_SHIFT_M2_M1 && !TUNE_M0_M8_MEGA_FEB
                    else if (enc_mode <= ENC_M1)
#else
                    else if (enc_mode <= ENC_M2)
#endif
#else
                    else if (enc_mode <= ENC_M1)
#endif
#else
                    else if (enc_mode <= ENC_M2)
#endif
                        context_ptr->sq_weight = 95;
                    else
                        context_ptr->sq_weight = 90;

#if FTR_NSQ_RED_USING_RECON
        // max_part0_to_part1_dev is used to:
        // (1) skip the H_Path if the deviation between the Parent-SQ src-to-recon distortion of (1st quadrant + 2nd quadrant) and the Parent-SQ src-to-recon distortion of (3rd quadrant + 4th quadrant) is less than TH,
        // (2) skip the V_Path if the deviation between the Parent-SQ src-to-recon distortion of (1st quadrant + 3rd quadrant) and the Parent-SQ src-to-recon distortion of (2nd quadrant + 4th quadrant) is less than TH.
        if (pd_pass == PD_PASS_0)
            context_ptr->max_part0_to_part1_dev = 0;
        else if (pd_pass == PD_PASS_1)
            context_ptr->max_part0_to_part1_dev = 0;
        else
#if TUNE_M4_FEATURES
#if TUNE_SHIFT_PRESETS_DOWN && !TUNE_M0_M8_MEGA_FEB
            if (enc_mode <= ENC_M2)
#else
            if (enc_mode <= ENC_M3)
#endif
#else
            if (enc_mode <= ENC_M4)
#endif
                context_ptr->max_part0_to_part1_dev = 0;
#if TUNE_M4_FEATURES && !TUNE_M0_M8_MEGA_FEB
#if TUNE_SHIFT_PRESETS_DOWN
            else if (enc_mode <= ENC_M3)
#else
            else if (enc_mode <= ENC_M4)
#endif
#if TUNE_M4_BASE_NBASE
                context_ptr->max_part0_to_part1_dev = (pcs_ptr->temporal_layer_index == 0) ? 0 : 50;
#else
                context_ptr->max_part0_to_part1_dev = (pcs_ptr->temporal_layer_index == 0) ? 0 : 100;
#endif
#endif
            else
                context_ptr->max_part0_to_part1_dev = 100;
#endif

#if !FTR_NEW_CYCLES_ALLOC
        if (pd_pass < PD_PASS_2)
            context_ptr->switch_md_mode_based_on_sq_coeff = 0;
        else if (pcs_ptr->slice_type == I_SLICE)
            context_ptr->switch_md_mode_based_on_sq_coeff = 0;
        else if (enc_mode <= ENC_MR)
            context_ptr->switch_md_mode_based_on_sq_coeff = 0;
        else if (enc_mode <= ENC_M0)
            context_ptr->switch_md_mode_based_on_sq_coeff = 1;
        else if (enc_mode <= ENC_M1)
            context_ptr->switch_md_mode_based_on_sq_coeff = 2;
        else if (enc_mode <= ENC_M2)
            context_ptr->switch_md_mode_based_on_sq_coeff = pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 2 : 3;
        else
            context_ptr->switch_md_mode_based_on_sq_coeff = 3;


        coeff_based_switch_md_controls(context_ptr, context_ptr->switch_md_mode_based_on_sq_coeff);
#endif
    // Set pic_obmc_level @ MD
    if (pd_pass == PD_PASS_0)
        context_ptr->md_pic_obmc_level = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_pic_obmc_level = 0;
    else
        context_ptr->md_pic_obmc_level =
        pcs_ptr->parent_pcs_ptr->pic_obmc_level;

    set_obmc_controls(context_ptr, context_ptr->md_pic_obmc_level);

    // Set enable_inter_intra @ MD
    //Block level switch, has to follow the picture level
    // inter intra pred                      Settings
    // 0                                     OFF
    // 1                                     FULL
    // 2                                     FAST 1 : Do not inject for unipred3x3 or PME inter candidates
    // 3                                     FAST 2 : Level 1 + do not inject for non-closest ref frames or ref frames with high distortion
    if (pcs_ptr->parent_pcs_ptr->slice_type != I_SLICE && sequence_control_set_ptr->seq_header.enable_interintra_compound) {
        if (pd_pass == PD_PASS_0)
            context_ptr->md_inter_intra_level = 0;
        else if (pd_pass == PD_PASS_1)
            context_ptr->md_inter_intra_level = 0;
#if TUNE_PRESETS_CLEANUP
#if FTR_NEW_REF_PRUNING_CTRLS // intra-inter
#if TUNE_M3_REPOSITION
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M2)
#else
        else if (enc_mode <= ENC_M3)
#endif
#else
        else if (enc_mode <= ENC_M2)
#endif
#else
        else if (enc_mode <= ENC_M0)
#endif
#else
        else if (enc_mode <= ENC_M1)
#endif
#if FTR_NEW_REF_PRUNING_CTRLS // intra-inter
            context_ptr->md_inter_intra_level = 1;
#else
            context_ptr->md_inter_intra_level = 2;
        else if (enc_mode <= ENC_M2)
            context_ptr->md_inter_intra_level = 3;
#endif
        else
            context_ptr->md_inter_intra_level = 0;
    }
    else
        context_ptr->md_inter_intra_level = 0;

    set_inter_intra_ctrls(context_ptr, context_ptr->md_inter_intra_level);
    // Set enable_paeth @ MD
    if (pd_pass == PD_PASS_0)
        context_ptr->md_enable_paeth = 1;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_enable_paeth = 1;
    else if (pcs_ptr->parent_pcs_ptr->scs_ptr->static_config.enable_paeth == DEFAULT)
        context_ptr->md_enable_paeth = 1;
    else
        context_ptr->md_enable_paeth = (uint8_t)pcs_ptr->parent_pcs_ptr->scs_ptr->static_config.enable_paeth;

    // Set enable_smooth @ MD
    if (pd_pass == PD_PASS_0)
        context_ptr->md_enable_smooth = 1;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_enable_smooth = 1;
    else if (pcs_ptr->parent_pcs_ptr->scs_ptr->static_config.enable_smooth == DEFAULT)
        context_ptr->md_enable_smooth = 1;
    else
        context_ptr->md_enable_smooth = (uint8_t)pcs_ptr->parent_pcs_ptr->scs_ptr->static_config.enable_smooth;

    // Set md_tx_size_search_mode @ MD
    if (pd_pass == PD_PASS_0)
        context_ptr->md_tx_size_search_mode = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_tx_size_search_mode = 0;
    else
        context_ptr->md_tx_size_search_mode = pcs_ptr->parent_pcs_ptr->tx_size_search_mode;

    // Assign whether to use TXS in inter classes (if TXS is ON)
    // 0 OFF - Use TXS for intra candidates only
    // 1 ON  - Use TXS for all candidates
    // 2 ON  - INTER TXS restricted to max 1 depth
    if (enc_mode <= ENC_MRS)
        context_ptr->md_staging_tx_size_level = 1;
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_M0_REPOSITION
    else if (enc_mode <= ENC_MR)
#else
    else if (enc_mode <= ENC_M0)
#endif
#else
    else if (enc_mode <= ENC_M1)
#endif
        context_ptr->md_staging_tx_size_level = 2;
#if TUNE_M0_M3_BASE_NBASE
    else if (enc_mode <= ENC_M1)
        context_ptr->md_staging_tx_size_level = (pcs_ptr->temporal_layer_index == 0) ? 2 : 0;
#endif
    else
        context_ptr->md_staging_tx_size_level = 0;
#if CLN_MD_CAND_BUFF
    uint8_t nic_scaling_level = get_nic_scaling_level (pd_pass , enc_mode, pcs_ptr->temporal_layer_index);

#else
    uint8_t nic_scaling_level = 1;
    if (pd_pass == PD_PASS_0)
#if TUNE_PRESETS_AND_PRUNING
        nic_scaling_level = 15;
#else
        nic_scaling_level = 14;
#endif
    else if (pd_pass == PD_PASS_1)
        nic_scaling_level = 12;
    else
        if (enc_mode <= ENC_MR)
            nic_scaling_level = 0;
#if TUNE_MR_M0_FEATURES
#if !TUNE_M0_REPOSITION
        else if (enc_mode <= ENC_M0)
#if TUNE_M0_M3_BASE_NBASE
            nic_scaling_level = (pcs_ptr->temporal_layer_index == 0) ? 0 : 1;
#else
            nic_scaling_level = 1;
#endif
#endif
#endif
#if TUNE_NEW_PRESETS_MR_M8
        else if (enc_mode <= ENC_M1)
#else
        else if (enc_mode <= ENC_M0)
#endif
#if TUNE_MR_M0_FEATURES
#if TUNE_M0_M3_BASE_NBASE
            nic_scaling_level = (pcs_ptr->temporal_layer_index == 0) ? 1 : 2;
#if !TUNE_SHIFT_M2_M1
        else if (enc_mode <= ENC_M2)
            nic_scaling_level = (pcs_ptr->temporal_layer_index == 0) ? 2 : 6;
#endif
#else
            nic_scaling_level = 2;
#endif
#else
            nic_scaling_level = 1;
#endif
#if TUNE_LOWER_PRESETS
#if !TUNE_NEW_PRESETS_MR_M8
        else if (enc_mode <= ENC_M2)
            nic_scaling_level = 2;
#endif
#if TUNE_M5_FEATURES
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M3)
#else
        else if (enc_mode <= ENC_M4)
#endif
            nic_scaling_level = 6;
#endif
#if TUNE_M4_M8
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M4)
#else
        else if (enc_mode <= ENC_M5)
#endif
#else
        else if (enc_mode <= ENC_M3)
#endif
#if TUNE_M5_FEATURES
            nic_scaling_level = 8;
#else
            nic_scaling_level = 6;
#endif
#if !TUNE_NEW_PRESETS_MR_M8
        else if (enc_mode <= ENC_M5)
            nic_scaling_level = 7;
#endif
#else
        else if (enc_mode <= ENC_M5)
            nic_scaling_level = 6;
#endif
#else
        else if (enc_mode <= ENC_M1)
            nic_scaling_level = 4;
        else if (enc_mode <= ENC_M2)
            nic_scaling_level = 5;
        else if (enc_mode <= ENC_M3)
            nic_scaling_level = 7;
#endif
#if TUNE_PRESETS_AND_PRUNING
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M5)
#else
        else if (enc_mode <= ENC_M6)
#endif
            nic_scaling_level = 10;
#endif
#if FTR_NIC_PRUNING
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M6)
#else
        else if (enc_mode <= ENC_M7)
#endif
#else
        else if (enc_mode <= ENC_M6)
#endif
            nic_scaling_level = 11;
#if TUNE_PRESETS_AND_PRUNING
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M7)
#else
        else if (enc_mode <= ENC_M8)
#endif
#else
        else
#endif
            nic_scaling_level = 12;
#if TUNE_PRESETS_AND_PRUNING
#if FTR_M10
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M8)
#else
        else if(enc_mode <= ENC_M9)
#endif
#else
        else
#endif
#if OPT_M9_TXT_PRED_DEPTH_PRUNING
#if TUNE_M7_M9
            nic_scaling_level = 14;
#if FTR_M10
        else
            nic_scaling_level = 15;
#endif
#else
            nic_scaling_level = 15;
#endif
#else
            nic_scaling_level = 14;
#endif
#endif
#endif
    set_nic_controls(context_ptr, nic_scaling_level);

#if FTR_NIC_PRUNING
    uint8_t nic_pruning_level;
    if (pd_pass == PD_PASS_0)
        nic_pruning_level = 0;
    else if (pd_pass == PD_PASS_1)
        nic_pruning_level = 0;
    else
        if (enc_mode <= ENC_MRS)
            nic_pruning_level = 0;
        else if (enc_mode <= ENC_MR)
            nic_pruning_level = 1;
#if !TUNE_NEW_PRESETS_MR_M8
#if TUNE_LOWER_PRESETS
        else if (enc_mode <= ENC_M0)
#else
        else if (enc_mode <= ENC_M1)
#endif
            nic_pruning_level = 2;
#endif
#if TUNE_LOWER_PRESETS
#if TUNE_NEW_PRESETS_MR_M8
        else if (enc_mode <= ENC_M0)
#else
        else if (enc_mode <= ENC_M1)
#endif
#if TUNE_M0_M3_BASE_NBASE
            nic_pruning_level = (pcs_ptr->temporal_layer_index == 0) ? 1 : 3;
#if TUNE_M0_M8_MEGA_FEB
        else if (enc_mode <= ENC_M4)
#else
        else if (enc_mode <= ENC_M1)
#endif
            nic_pruning_level = (pcs_ptr->temporal_layer_index == 0) ? 3 : 4;
#else
            nic_pruning_level = 3;
#endif
#if !TUNE_M0_M8_MEGA_FEB
#if TUNE_M3_REPOSITION
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M2)
#else
        else if (enc_mode <= ENC_M3)
#endif
            nic_pruning_level = 4;
#else
        else if (enc_mode <= ENC_M2)
            nic_pruning_level = 4;
#endif
#endif
#if TUNE_M0_M3_BASE_NBASE
#if TUNE_M4_REPOSITION
#if TUNE_SHIFT_PRESETS_DOWN
#if NEW_PRESETS
#if TUNE_M0_M8_MEGA_FEB
#if TUNE_FINAL_M4_M8
        else if (enc_mode <= ENC_M6)
#else
        else if (enc_mode <= ENC_M5)
#endif
#else
        else if (enc_mode <= ENC_M4)
#endif
#else
        else if (enc_mode <= ENC_M3)
#endif
#else
        else if (enc_mode <= ENC_M4)
#endif
#else
        else if (enc_mode <= ENC_M3)
#endif
            nic_pruning_level = (pcs_ptr->temporal_layer_index == 0) ? 4 : 5;
#endif
#if !TUNE_M0_M8_MEGA_FEB
#if TUNE_PRESETS_AND_PRUNING
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M5)
#else
        else if (enc_mode <= ENC_M6)
#endif
#else
        else if (enc_mode <= ENC_M5)
#endif
            nic_pruning_level = 5;
#endif
#else
        else if (enc_mode <= ENC_M5)
            nic_pruning_level = 3;
#endif
#if !TUNE_FINAL_M4_M8
#if TUNE_M4_M8
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M6)
#else
        else if (enc_mode <= ENC_M7)
#endif
#else
        else if (enc_mode <= ENC_M6)
#endif
#if TUNE_LOWER_PRESETS
            nic_pruning_level = 6;
#else
            nic_pruning_level = 4;
#endif
#endif
#if TUNE_PRESETS_AND_PRUNING
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M7)
#else
        else if (enc_mode <= ENC_M8)
#endif
#else
        else
#endif
#if TUNE_LOWER_PRESETS
#if TUNE_M4_M8
            nic_pruning_level = (sequence_control_set_ptr->input_resolution <= INPUT_SIZE_480p_RANGE) ? 7 : 8;
#else
            nic_pruning_level = 7;
#endif
#else
            nic_pruning_level = 5;
#endif
#if TUNE_PRESETS_AND_PRUNING
#if TUNE_M10_NIC_PRUNING
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M8)
#else
        else if (enc_mode <= ENC_M9)
#endif
            nic_pruning_level = (sequence_control_set_ptr->input_resolution <= INPUT_SIZE_480p_RANGE) ? 9 : 10;
        else
            nic_pruning_level = 11;
#else
        else
        nic_pruning_level = (sequence_control_set_ptr->input_resolution <= INPUT_SIZE_480p_RANGE) ? 9 : 10;
#endif
#endif
    set_nic_pruning_controls(context_ptr, nic_pruning_level);
#endif
#if !CLN_NSQ_AND_STATS
    uint8_t txs_cycles_reduction_level = 0;
    set_txs_cycle_reduction_controls(context_ptr, txs_cycles_reduction_level);
#endif
    // Set md_filter_intra_mode @ MD
    // md_filter_intra_level specifies whether filter intra would be active
    // for a given prediction candidate in mode decision.

    // md_filter_intra_level | Settings
    // 0                      | OFF
    // 1                      | ON
    if (pd_pass == PD_PASS_0)
        context_ptr->md_filter_intra_level = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_filter_intra_level = 0;
    else
        context_ptr->md_filter_intra_level =
        pcs_ptr->pic_filter_intra_level;
    // Set md_allow_intrabc @ MD
    if (pd_pass == PD_PASS_0)
        context_ptr->md_allow_intrabc = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_allow_intrabc = 0;
    else
        context_ptr->md_allow_intrabc = pcs_ptr->parent_pcs_ptr->frm_hdr.allow_intrabc;

    // Set md_palette_level @ MD
    if (pd_pass == PD_PASS_0)
        context_ptr->md_palette_level = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_palette_level = 0;
    else
        context_ptr->md_palette_level = pcs_ptr->parent_pcs_ptr->palette_level;
#if !FTR_EARLY_DEPTH_REMOVAL
    // Set block_based_depth_refinement_level
    if (enc_mode <= ENC_M5)
        context_ptr->block_based_depth_refinement_level = 0;
    else if (enc_mode <= ENC_M6)
        context_ptr->block_based_depth_refinement_level = 2;
#if FTR_PD2_BLOCK_REDUCTION
    else if (enc_mode <= ENC_M7) {
#else
    else {
#endif
        if (pcs_ptr->slice_type == I_SLICE) {
            context_ptr->block_based_depth_refinement_level = 4;
        }
        else {
            context_ptr->block_based_depth_refinement_level = 5;
        }
    }
#if FTR_PD2_BLOCK_REDUCTION
    else {
        if (pcs_ptr->slice_type == I_SLICE) {
            context_ptr->block_based_depth_refinement_level = 4;
        }
        else {
            context_ptr->block_based_depth_refinement_level = 6;
        }
    }
#endif
    set_block_based_depth_refinement_controls(sequence_control_set_ptr, pcs_ptr, context_ptr, context_ptr->block_based_depth_refinement_level, sb_width, sb_height);

#endif
    if (pcs_ptr->slice_type != I_SLICE) {
#if FTR_PF_PD0_DETECTOR
        if (pd_pass == PD_PASS_0) {

            // Only allow PF if using SB_64x64
#if TUNE_M4_M8
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_M7_M9
#if TUNE_M6_FEATURES && !TUNE_FINAL_M4_M8
#if TUNE_SHIFT_PRESETS_DOWN && !TUNE_M0_M8_MEGA_FEB
            if (enc_mode <= ENC_M4 ||
#else
            if (enc_mode <= ENC_M5 ||
#endif
#else
            if (enc_mode <= ENC_M6 ||
#endif
#else
            if (enc_mode <= ENC_M7 ||
#endif
#else
            if (enc_mode <= ENC_M6 ||
#endif
#else
            if (enc_mode <= ENC_M7 ||
#endif
                pcs_ptr->parent_pcs_ptr->input_resolution <= INPUT_SIZE_480p_RANGE ||
                sequence_control_set_ptr->static_config.super_block_size == 128) {
                context_ptr->pf_level = 1;
            }
            else {
                // Use ME distortion and variance detector to enable PF
                uint32_t fast_lambda = context_ptr->hbd_mode_decision ?
                    context_ptr->fast_lambda_md[EB_10_BIT_MD] :
                    context_ptr->fast_lambda_md[EB_8_BIT_MD];
                uint32_t sb_size = sequence_control_set_ptr->static_config.super_block_size * sequence_control_set_ptr->static_config.super_block_size;
                uint64_t cost_th_rate = 1 << 13;
                uint64_t use_pf_th = 0;

                if (pcs_ptr->parent_pcs_ptr->variance[context_ptr->sb_index][ME_TIER_ZERO_PU_64x64] <= 400)
                    use_pf_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 2);
                else if (pcs_ptr->parent_pcs_ptr->variance[context_ptr->sb_index][ME_TIER_ZERO_PU_64x64] <= 800)
                    use_pf_th = RDCOST(fast_lambda, cost_th_rate, sb_size);
                else
                    use_pf_th = RDCOST(fast_lambda, cost_th_rate, sb_size >> 1);

                uint64_t cost_64x64 = RDCOST(fast_lambda, 0, pcs_ptr->parent_pcs_ptr->me_64x64_distortion[context_ptr->sb_index]);
#if TUNE_M9_PF
#if TUNE_M8_FEATURES && !TUNE_M0_M8_MEGA_FEB
#if TUNE_SHIFT_PRESETS_DOWN
#if NEW_PRESETS
                if (enc_mode <= ENC_M7) {
#else
                if (enc_mode <= ENC_M6) {
#endif
#else
                if (enc_mode <= ENC_M7) {
#endif
#else
                if (enc_mode <= ENC_M8) {
#endif
                    context_ptr->pf_level = (cost_64x64 < use_pf_th) ? 3 : 1;
                }
                else {
                    if (pcs_ptr->parent_pcs_ptr->input_resolution <= INPUT_SIZE_720p_RANGE)
                        context_ptr->pf_level = (cost_64x64 < ((use_pf_th * 3) >> 1)) ? 3 : 1;
                    else
                        context_ptr->pf_level = (cost_64x64 < ((use_pf_th * 5) >> 1)) ? 3 : 1;
                }
#else
                context_ptr->pf_level = (cost_64x64 < use_pf_th) ? 3 : 1;
#endif
            }
        }
#else
        if (pd_pass == PD_PASS_0)
            context_ptr->pf_level = 1;
#endif
        else if (pd_pass == PD_PASS_1)
            context_ptr->pf_level = 1;
        else
            context_ptr->pf_level = 1;
    } else {
        context_ptr->pf_level = 1;
    }
    set_pf_controls(context_ptr, context_ptr->pf_level);
#if OPT_REFACTOR_IN_DEPTH_CTRLS
    uint8_t in_depth_block_skip_level = 0;
#if FTR_ALIGN_SC_DETECOR
    if (pcs_ptr->parent_pcs_ptr->sc_class1)
#else
    if (pcs_ptr->parent_pcs_ptr->sc_content_detected)
#endif
        in_depth_block_skip_level = 0;
    else if (context_ptr->pd_pass == PD_PASS_0)
        if (enc_mode <= ENC_M9)
            in_depth_block_skip_level = 0;
        else
#if OPT_IN_DEPTH_SKIP_M9
            in_depth_block_skip_level = (pcs_ptr->parent_pcs_ptr->temporal_layer_index == 0) ? 0 : 1;
#else
            in_depth_block_skip_level = (pcs_ptr->parent_pcs_ptr->temporal_layer_index == 0) ? 0 : 2;
#endif
    else if (context_ptr->pd_pass == PD_PASS_1)
        in_depth_block_skip_level = 0;
    else
        in_depth_block_skip_level = 0;

    set_in_depth_block_skip_ctrls(context_ptr, in_depth_block_skip_level);
#else
#if TUNE_M10_MD_EXIT
    // Derive MD Exit TH
    if (context_ptr->pd_pass == PD_PASS_0)
#if TUNE_SHIFT_PRESETS_DOWN
        if (enc_mode <= ENC_M8) {
#else
        if (enc_mode <= ENC_M9) {
#endif
            context_ptr->md_exit_th = 0;
        }
        else {
            context_ptr->md_exit_th = 75;
        }
    else if (context_ptr->pd_pass == PD_PASS_1)
        context_ptr->md_exit_th = 0;
    else
        context_ptr->md_exit_th = 0;
#endif
#endif
#if LOWER_DEPTH_EXIT_CTRL
    uint8_t lower_depth_block_skip_level = 0;
#if FTR_ALIGN_SC_DETECOR
    if (pcs_ptr->parent_pcs_ptr->sc_class1)
#else
    if (pcs_ptr->parent_pcs_ptr->sc_content_detected)
#endif
        lower_depth_block_skip_level = 0;
    else if (pd_pass == PD_PASS_0)
        lower_depth_block_skip_level = 0;
    else if (pd_pass == PD_PASS_1)
        lower_depth_block_skip_level = 0;
    else {
        if (enc_mode <= ENC_M7)
            lower_depth_block_skip_level = 0;
        else if (enc_mode <= ENC_M9)
            lower_depth_block_skip_level = 1;
        else
            lower_depth_block_skip_level = 2;
    }

    set_lower_depth_block_skip_ctrls(context_ptr, lower_depth_block_skip_level);
#else
#if FTR_IMPROVE_DEPTH_REMOVAL
    uint8_t depth_skip_level;
#if FTR_ALIGN_SC_DETECOR
    if (pcs_ptr->parent_pcs_ptr->sc_class1)
#else
    if (pcs_ptr->parent_pcs_ptr->sc_content_detected)
#endif
        depth_skip_level = 0;
    else if (pd_pass == PD_PASS_0)
        depth_skip_level = 0;
    else if (pd_pass == PD_PASS_1)
        depth_skip_level = 0;
    else
#if TUNE_M7_M9
#if TUNE_SHIFT_PRESETS_DOWN
#if NEW_PRESETS
        if (enc_mode <= ENC_M7)
#else
        if (enc_mode <= ENC_M6)
#endif
#else
        if (enc_mode <= ENC_M7)
#endif
#else
        if (enc_mode <= ENC_M6)
#endif
            depth_skip_level = 0;
#if TUNE_M9_FEATURES
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M8)
#else
        else  if (enc_mode <= ENC_M9)
#endif
#else
        else  if (enc_mode <= ENC_M8)
#endif
            depth_skip_level = 1;
        else
            depth_skip_level = 2;
    set_depth_skip_controls(context_ptr, depth_skip_level);
#endif
#endif
#if FEATURE_IMPROVE_DEPTH_REMOVAL
    uint8_t stage2_depth_reduction_level;
    if (pcs_ptr->parent_pcs_ptr->sc_content_detected)
        stage2_depth_reduction_level = 0;
    else if (pd_pass == PD_PASS_0)
        stage2_depth_reduction_level = 0;
    else if (pd_pass == PD_PASS_1)
        stage2_depth_reduction_level = 0;
    else
#if TUNE_M7_FEATURES
        if (enc_mode <= ENC_M7)
#else
        if (enc_mode <= ENC_M6)
#endif
            stage2_depth_reduction_level = 0;
#if TUNE_M9_FEATURES
#if FTR_M10
        else if (enc_mode <= ENC_M10)
#else
        else  if (enc_mode <= ENC_M9)
#endif
#else
        else  if (enc_mode <= ENC_M8)
#endif
            stage2_depth_reduction_level = 1;
        else
            stage2_depth_reduction_level = 2;
    set_stage2_depth_reduction_controls(context_ptr, stage2_depth_reduction_level);
#endif
    if (pd_pass == PD_PASS_0)
        context_ptr->md_sq_mv_search_level = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_sq_mv_search_level = 0;
    else
#if TUNE_SHIFT_M2_M1
#if TUNE_M0_M8_MEGA_FEB
        if (enc_mode <= ENC_M3)
#else
        if (enc_mode <= ENC_M1)
#endif
#else
        if (enc_mode <= ENC_M2)
#endif
            context_ptr->md_sq_mv_search_level = 1;
#if TUNE_M4_M8
#if !TUNE_NEW_PRESETS_MR_M8
        else if (enc_mode <= ENC_M3)
            context_ptr->md_sq_mv_search_level = 2;
#endif
#if TUNE_M9_IFS_SSE_ADAPT_ME_MV_NEAR_WM_TF
#if TUNE_M7_M9
#if TUNE_M6_FEATURES && !TUNE_M0_M8_MEGA_FEB
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M4)
#else
        else if (enc_mode <= ENC_M5)
#endif
#else
#if TUNE_FINAL_M4_M8
        else if (enc_mode <= ENC_M5)
#else
        else if (enc_mode <= ENC_M6)
#endif
#endif
#else
        else if (enc_mode <= ENC_M8)
#endif
            context_ptr->md_sq_mv_search_level = 4;
        else
            context_ptr->md_sq_mv_search_level = 0;
#else
        else
            context_ptr->md_sq_mv_search_level = 4;
#endif
#else
        else if (enc_mode <= ENC_M4)
            context_ptr->md_sq_mv_search_level = 2;
#if FTR_DISABLE_ADAPTIVE_ME
        else if (enc_mode <= ENC_M7)
            context_ptr->md_sq_mv_search_level = 4;
        else
            context_ptr->md_sq_mv_search_level = 0;
#else
        else
            context_ptr->md_sq_mv_search_level = 4;
#endif
#endif
    md_sq_motion_search_controls(context_ptr, context_ptr->md_sq_mv_search_level);
    if (pd_pass == PD_PASS_0)
        context_ptr->md_nsq_mv_search_level = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_nsq_mv_search_level = 0;
    else
#if TUNE_PRESETS_CLEANUP
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_MR_M0_FEATURES
        if (enc_mode <= ENC_MRS)
#else
        if (enc_mode <= ENC_MR)
#endif
#else
        if (enc_mode <= ENC_M0)
#endif
#else
        if (enc_mode <= ENC_MRS)
            context_ptr->md_nsq_mv_search_level = 1;
        else if (enc_mode <= ENC_M1)
#endif
            context_ptr->md_nsq_mv_search_level = 2;
        else
            context_ptr->md_nsq_mv_search_level = 4;

    md_nsq_motion_search_controls(context_ptr, context_ptr->md_nsq_mv_search_level);
    // Set PME level
    if (pd_pass == PD_PASS_0)
        context_ptr->md_pme_level = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_pme_level = 3;
    else
#if TUNE_LOWER_PRESETS
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_M1_FEATURES
#if TUNE_M1_REPOSITION
        if (enc_mode <= ENC_M0)
#else
        if (enc_mode <= ENC_M1)
#endif
#else
        if (enc_mode <= ENC_M0)
#endif
#else
        if (enc_mode <= ENC_M1)
#endif
#else
        if (enc_mode <= ENC_M2)
#endif
            context_ptr->md_pme_level = 1;
#if TUNE_SHIFT_PRESETS_DOWN
#if NEW_PRESETS && !TUNE_FINAL_M4_M8
        else if (enc_mode <= ENC_M6)
#else
        else if (enc_mode <= ENC_M5)
#endif
#else
        else if (enc_mode <= ENC_M6)
#endif
            context_ptr->md_pme_level = 2;
#if TUNE_M9_PME
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M7)
#else
        else if (enc_mode <= ENC_M8)
#endif
            context_ptr->md_pme_level = 3;
#if FTR_M10
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M8)
#else
        else if (enc_mode <= ENC_M9)
#endif
            context_ptr->md_pme_level = 4;
        else
            context_ptr->md_pme_level = 0;
#else
        else
            context_ptr->md_pme_level = 4;
#endif
#else
        else
            context_ptr->md_pme_level = 3;
#endif
    md_pme_search_controls(context_ptr, context_ptr->md_pme_level);

    if (pd_pass == PD_PASS_0)
#if TUNE_M4_M8
#if TUNE_M6_M7_FEATURES
#if TUNE_SHIFT_PRESETS_DOWN
#if NEW_PRESETS && !TUNE_M0_M8_MEGA_FEB
        context_ptr->md_subpel_me_level = enc_mode <= ENC_M6 ? 3 : 0;
#else
        context_ptr->md_subpel_me_level = enc_mode <= ENC_M5 ? 3 : 0;
#endif
#else
        context_ptr->md_subpel_me_level = enc_mode <= ENC_M6 ? 3 : 0;
#endif
#else
        context_ptr->md_subpel_me_level = enc_mode <= ENC_M5 ? 3 : 0;
#endif
#else
        context_ptr->md_subpel_me_level = enc_mode <= ENC_M4 ? 3 : 0;
#endif
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_subpel_me_level = 3;
    else
#if TUNE_M4_M8
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_M7_M9
#if TUNE_M6_FEATURES
#if TUNE_SHIFT_PRESETS_DOWN
        if (enc_mode <= ENC_M4)
#else
        if (enc_mode <= ENC_M5)
#endif
#else
        if (enc_mode <= ENC_M6)
#endif
#else
        if (enc_mode <= ENC_M7)
#endif
#else
        if (enc_mode <= ENC_M5)
#endif
#else
        if (enc_mode <= ENC_M4)
#endif
            context_ptr->md_subpel_me_level = 1;
#if FTR_PRUNED_SUBPEL_TREE
#if TUNE_SHIFT_PRESETS_DOWN
#if TUNE_FINAL_M4_M8
        else if (enc_mode <= ENC_M6)
#else
        else if (enc_mode <= ENC_M7)
#endif
#else
        else if (enc_mode <= ENC_M8)
#endif
#else
        else
#endif
#if TUNE_NEW_PRESETS_MR_M8
            context_ptr->md_subpel_me_level = pcs_ptr->parent_pcs_ptr->input_resolution <= INPUT_SIZE_480p_RANGE ? 1 : 2;
#if FTR_PRUNED_SUBPEL_TREE
#if TUNE_M10_SUBPEL
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M8)
#else
        else if (enc_mode <= ENC_M9)
#endif
        context_ptr->md_subpel_me_level = pcs_ptr->parent_pcs_ptr->input_resolution <= INPUT_SIZE_480p_RANGE ? 4 : 5;
        else
        context_ptr->md_subpel_me_level = 6;
#else
        else
            context_ptr->md_subpel_me_level = pcs_ptr->parent_pcs_ptr->input_resolution <= INPUT_SIZE_480p_RANGE ? 4 : 5;
#endif
#endif
#else
            context_ptr->md_subpel_me_level = 2;
#endif

    md_subpel_me_controls(context_ptr, context_ptr->md_subpel_me_level);

    if (pd_pass == PD_PASS_0)
#if TUNE_M4_M8
#if TUNE_SHIFT_PRESETS_DOWN
        context_ptr->md_subpel_pme_level = enc_mode <= ENC_M4 ? 3 : 0;
#else
        context_ptr->md_subpel_pme_level = enc_mode <= ENC_M5 ? 3 : 0;
#endif
#else
        context_ptr->md_subpel_pme_level = enc_mode <= ENC_M4 ? 3 : 0;
#endif
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_subpel_pme_level = 3;
#if FTR_PRUNED_SUBPEL_TREE
#if TUNE_SHIFT_PRESETS_DOWN
    else if (enc_mode <= ENC_M7)
#else
    else if (enc_mode <= ENC_M8)
#endif
        context_ptr->md_subpel_pme_level = 1;
    else
        context_ptr->md_subpel_pme_level = 2;
#else
    else
#if TUNE_M4_M8
        context_ptr->md_subpel_pme_level = 1;
#else
        if (enc_mode <= ENC_M6)
            context_ptr->md_subpel_pme_level = 1;
        else
            context_ptr->md_subpel_pme_level = 2;
#endif
#endif
    md_subpel_pme_controls(context_ptr, context_ptr->md_subpel_pme_level);
#if !FTR_NEW_REF_PRUNING_CTRLS
    // Set max_ref_count @ MD
    if (pd_pass == PD_PASS_0)
        context_ptr->md_max_ref_count = 4;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_max_ref_count = 1;
    else
        context_ptr->md_max_ref_count = 4;
#endif
    // Set dc_cand_only_flag
    if (pd_pass == PD_PASS_0)
        context_ptr->dc_cand_only_flag = EB_TRUE;
    else if (pd_pass == PD_PASS_1)
        context_ptr->dc_cand_only_flag =
        (pcs_ptr->slice_type == I_SLICE) ? EB_FALSE : EB_TRUE;
    else
#if CLN_ENC_MODE_CHECK
#if TUNE_M8_TO_MATCH_M7
#if TUNE_M9_FEATURES && !TUNE_SHIFT_PRESETS_DOWN
        if (enc_mode <= ENC_M9)
#else
        if (enc_mode <= ENC_M8)
#endif
#else
        if (enc_mode <= ENC_M7)
#endif
#else
        if (enc_mode < ENC_M8)
#endif
            context_ptr->dc_cand_only_flag = EB_FALSE;
        else
#if FTR_PD2_REDUCE_INTRA
#if TUNE_M10_USE_DC_ONLY
            context_ptr->dc_cand_only_flag = EB_TRUE;
#else
            context_ptr->dc_cand_only_flag = !pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ||
            ( (pcs_ptr->slice_type != I_SLICE) && (pcs_ptr->parent_pcs_ptr->rc_me_distortion[context_ptr->sb_index] < (64 * 64 * 3)))
                ? EB_TRUE
                : EB_FALSE;
#endif
#else
            context_ptr->dc_cand_only_flag = !pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag
                ? EB_TRUE
                : EB_FALSE;
#endif

    // Set intra_angle_delta @ MD
    if (pd_pass == PD_PASS_0)
        context_ptr->md_intra_angle_delta = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->md_intra_angle_delta = 0;
    else if (pcs_ptr->parent_pcs_ptr->scs_ptr->static_config.intra_angle_delta == DEFAULT)
        context_ptr->md_intra_angle_delta = 1;
    else
        context_ptr->md_intra_angle_delta = pcs_ptr->parent_pcs_ptr->scs_ptr->static_config.intra_angle_delta;

    // Set disable_angle_z2_prediction_flag
    if (pd_pass == PD_PASS_0)
        context_ptr->disable_angle_z2_intra_flag = EB_TRUE;
    else if (pd_pass == PD_PASS_1)
        context_ptr->disable_angle_z2_intra_flag = EB_TRUE;
    else
        context_ptr->disable_angle_z2_intra_flag = EB_FALSE;
    // Shut skip_context and dc_sign update for rate estimation
    if (pd_pass == PD_PASS_0)
#if TUNE_M4_M8
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_SHIFT_PRESETS_DOWN
        context_ptr->shut_skip_ctx_dc_sign_update = enc_mode <= ENC_M4 ? EB_FALSE : EB_TRUE;
#else
        context_ptr->shut_skip_ctx_dc_sign_update = enc_mode <= ENC_M5 ? EB_FALSE : EB_TRUE;
#endif
#else
        context_ptr->shut_skip_ctx_dc_sign_update = enc_mode <= ENC_M4 ? EB_FALSE : EB_TRUE;
#endif
#else
        context_ptr->shut_skip_ctx_dc_sign_update = enc_mode <= ENC_M5 ? EB_FALSE : EB_TRUE;
#endif
    else if (pd_pass == PD_PASS_1)
#if TUNE_M9_SKIP_CTX_DC_SIGN
        context_ptr->shut_skip_ctx_dc_sign_update = EB_TRUE;
#else
        context_ptr->shut_skip_ctx_dc_sign_update = EB_FALSE;
#endif
    else
#if TUNE_M9_SKIP_CTX_DC_SIGN
#if TUNE_SHIFT_PRESETS_DOWN
        context_ptr->shut_skip_ctx_dc_sign_update = enc_mode <= ENC_M7 ?
        EB_FALSE :
        (pcs_ptr->slice_type == I_SLICE) ?
        EB_FALSE :
        EB_TRUE;
#else
        context_ptr->shut_skip_ctx_dc_sign_update = enc_mode <= ENC_M8 ?
            EB_FALSE :
            (pcs_ptr->slice_type == I_SLICE) ?
                EB_FALSE :
                EB_TRUE;
#endif
#else
        context_ptr->shut_skip_ctx_dc_sign_update = EB_FALSE;
#endif
    // Use coeff rate and slit flag rate only (i.e. no fast rate)
    if (pd_pass == PD_PASS_0)
        context_ptr->shut_fast_rate = EB_TRUE;
    else if (pd_pass == PD_PASS_1)
        context_ptr->shut_fast_rate = EB_FALSE;
    else
        context_ptr->shut_fast_rate = EB_FALSE;
#if FTR_FAST_RATE_ESTIMATION
    // Estimate the rate of the first (eob/N) coeff(s) and last coeff only
    if (pd_pass == PD_PASS_0)
#if FTR_M10
#if TUNE_M8_FEATURES
#if TUNE_M6_M7_FEATURES
#if TUNE_SHIFT_PRESETS_DOWN
#if TUNE_M0_M8_MEGA_FEB
        if (enc_mode <= ENC_M3)
#else
        if (enc_mode <= ENC_M4)
#endif
#else
        if (enc_mode <= ENC_M5)
#endif
#else
        if (enc_mode <= ENC_M7)
#endif
#else
        if (enc_mode <= ENC_M8)
#endif
            context_ptr->fast_coeff_est_level = 0;
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M8)
#else
        else if (enc_mode <= ENC_M9)
#endif
            context_ptr->fast_coeff_est_level = 1;
        else
            context_ptr->fast_coeff_est_level = 2;
#else
        context_ptr->fast_coeff_est_level = (enc_mode <= ENC_M8) ? 0 : 1;
#endif
    else if (pd_pass == PD_PASS_1)
        context_ptr->fast_coeff_est_level = 0;
    else
        context_ptr->fast_coeff_est_level = 0;
#endif
    if (pcs_ptr->slice_type == I_SLICE)
        context_ptr->skip_intra = 0;
    else if (pd_pass == PD_PASS_0)
#if TUNE_LOWER_PRESETS
#if TUNE_SHIFT_M2_M1
        if (enc_mode <= ENC_M1)
#else
        if (enc_mode <= ENC_M2)
#endif
#else
        if (enc_mode <= ENC_M3)
#endif
            context_ptr->skip_intra = 0;
#if TUNE_M0_M3_BASE_NBASE
#if TUNE_M4_REPOSITION
#if TUNE_SHIFT_PRESETS_DOWN
#if NEW_PRESETS
#if TUNE_M0_M8_MEGA_FEB
        else if (enc_mode <= ENC_M7)
#else
        else if (enc_mode <= ENC_M4)
#endif
#else
        else if (enc_mode <= ENC_M3)
#endif
#else
        else if (enc_mode <= ENC_M4)
#endif
#else
        else if (enc_mode <= ENC_M3)
#endif
            context_ptr->skip_intra = (pcs_ptr->temporal_layer_index == 0) ? 0 : 1;
#endif
        else
            context_ptr->skip_intra = 1;
    else
        context_ptr->skip_intra = 0;
#if !CLN_REMOVE_UNUSED_SIGNALS
    // skip cfl based on inter/intra cost deviation (skip if intra_cost is
    // skip_cfl_cost_dev_th % greater than inter_cost)
    if (enc_mode <= ENC_MR)
        context_ptr->skip_cfl_cost_dev_th = (uint16_t)~0;
    else
        context_ptr->skip_cfl_cost_dev_th = 30;

    // set intra count to zero for md stage 3 if intra_cost is
    // mds3_intra_prune_th % greater than inter_cost
    if (enc_mode <= ENC_MR)
        context_ptr->mds3_intra_prune_th = (uint16_t)~0;
    else
        context_ptr->mds3_intra_prune_th = 30;
#endif
    if (pd_pass == PD_PASS_0)
        context_ptr->use_prev_mds_res = EB_FALSE;
    else if (pd_pass == PD_PASS_1)
        context_ptr->use_prev_mds_res = EB_FALSE;
    else
#if TUNE_M4_M8
        context_ptr->use_prev_mds_res = EB_FALSE;
#else
    context_ptr->use_prev_mds_res =
            (enc_mode <= ENC_M5 || pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag) ? EB_FALSE
                                                                                       : EB_TRUE;
#endif
    if (pd_pass == PD_PASS_0)
        context_ptr->early_cand_elimination = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->early_cand_elimination = 0;
    else
        if (pcs_ptr->slice_type == I_SLICE)
            context_ptr->early_cand_elimination = 0;
        else
#if TUNE_M4_M8
#if OPTIMISE_EARLY_CANDIDATE_ELIMINATION || FTR_M9_AGRESSIVE_EARLY_CAN_ELIMINATION
#if TUNE_M8_FEATURES
#if TUNE_SHIFT_PRESETS_DOWN
            if (enc_mode <= ENC_M6)
#else
            if (enc_mode <= ENC_M7)
#endif
#else
            if (enc_mode <= ENC_M8)
#endif
                context_ptr->early_cand_elimination = 0;
            else
                context_ptr->early_cand_elimination = pcs_ptr->parent_pcs_ptr->input_resolution <= INPUT_SIZE_480p_RANGE ? 120 : 102;
#else
            context_ptr->early_cand_elimination = 0;
#endif
#else
            context_ptr->early_cand_elimination = (enc_mode <= ENC_M6) ? 0 : 1;
#endif

#if FTR_REDUCE_MDS3_COMPLEXITY
    /*reduce_last_md_stage_candidate
    0: OFF
    1: Aply PFN2 when the block is 0 coeff and PFN4 when MDS0 cand == MDS1 cand and
    the candidate does not belong to the best class
    2: 1 + disallow RDOQ and IFS when when MDS0 cand == MDS1 cand and
    the candidate does not belong to the best class
    3: 1 + 2 + remouve candidates when when MDS0 cand == MDS1 cand and they don't belong to the best class*/
#endif
#if FTR_REDUCE_MDS2_CAND
     if (pd_pass == PD_PASS_0)
        context_ptr->reduce_last_md_stage_candidate = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->reduce_last_md_stage_candidate = 0;
    else
        if (pcs_ptr->slice_type == I_SLICE)
            context_ptr->reduce_last_md_stage_candidate = 0;
        else
#if TUNE_M4_M8
#if FTR_REDUCE_MDS3_COMPLEXITY
#if TUNE_NEW_PRESETS_MR_M8
#if FTR_M9_AGRESSIVE_LAST_MD_STAGE
#if TUNE_SHIFT_PRESETS_DOWN && !TUNE_M0_M8_MEGA_FEB
            context_ptr->reduce_last_md_stage_candidate = (enc_mode <= ENC_M7) ? 0 : 3;
#else
#if TUNE_FINAL_M4_M8
            context_ptr->reduce_last_md_stage_candidate = (enc_mode <= ENC_M7) ? 0 : 3;
#else
            context_ptr->reduce_last_md_stage_candidate = (enc_mode <= ENC_M8) ? 0 : 3;
#endif
#endif
#else
            context_ptr->reduce_last_md_stage_candidate = (enc_mode <= ENC_M8) ? 0 : 2;
#endif
#else
            context_ptr->reduce_last_md_stage_candidate = (enc_mode <= ENC_M7) ? 0 : 2;
#endif
#else
            context_ptr->reduce_last_md_stage_candidate = 0;
#endif
#else
            context_ptr->reduce_last_md_stage_candidate = (enc_mode <= ENC_M7) ? 0 : 1;
#endif
#endif
#if TUNE_M10_MERGE_INTER_CLASSES
     if (pd_pass == PD_PASS_0)
         context_ptr->merge_inter_classes = 1;
     else if (pd_pass == PD_PASS_1)
         context_ptr->merge_inter_classes = 1;
     else
#if TUNE_SHIFT_PRESETS_DOWN
         context_ptr->merge_inter_classes = (enc_mode <= ENC_M8) ? 0 : 1;
#else
         context_ptr->merge_inter_classes = (enc_mode <= ENC_M9) ? 0 : 1;
#endif
#endif
#if FTR_USE_VAR_IN_FAST_LOOP
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_PRESETS_AND_PRUNING
#if TUNE_M3_FEATURES
#if TUNE_M2_FEATURES
#if TUNE_M1_FEATURES
#if TUNE_MR_M0_FEATURES
     context_ptr->use_var_in_mds0 = (enc_mode <= ENC_MRS) ? 0 : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 1 : 0;
#else
     context_ptr->use_var_in_mds0 = (enc_mode <= ENC_M0) ? 0 : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 1 : 0;
#endif
#else
     context_ptr->use_var_in_mds0 = (enc_mode <= ENC_M1) ? 0 : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 1 : 0;
#endif
#else
     context_ptr->use_var_in_mds0 = (enc_mode <= ENC_M2) ? 0 : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 1 : 0;
#endif
#else
     context_ptr->use_var_in_mds0 = (enc_mode <= ENC_M3) ? 0 : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 1 : 0;
#endif
#else
     context_ptr->use_var_in_mds0 = (enc_mode <= ENC_M5) ? 0 : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 1 : 0;
#endif
#else
     context_ptr->use_var_in_mds0 = (enc_mode <= ENC_M7) ? 0 : pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ? 1 : 0;
#endif
#endif
#if CLN_CANDIDATE_ELEMINATION_CTR
     uint8_t eliminate_candidate_based_on_pme_me_results = 0;
#if FTR_PD2_REDUCE_MDS0
     if (pd_pass == PD_PASS_0)
        eliminate_candidate_based_on_pme_me_results = 0;
    else if (pd_pass == PD_PASS_1)
        eliminate_candidate_based_on_pme_me_results = 0;
    else
        if (pcs_ptr->slice_type == I_SLICE)
            eliminate_candidate_based_on_pme_me_results = 0;
#if OPT_WM
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M6)
#else
        else if(enc_mode <= ENC_M7)
#endif
            eliminate_candidate_based_on_pme_me_results = 0;
#if TUNE_SHIFT_PRESETS_DOWN
        else if (enc_mode <= ENC_M7)
#else
        else if(enc_mode <= ENC_M8)
#endif
            eliminate_candidate_based_on_pme_me_results = 1;
        else
            eliminate_candidate_based_on_pme_me_results = 2;
#else
        else
#if TUNE_M4_M8
#if TUNE_M7_M9
            eliminate_candidate_based_on_pme_me_results = (enc_mode <= ENC_M7) ? 0 : 1;
#else
            eliminate_candidate_based_on_pme_me_results = (enc_mode <= ENC_M6) ? 0 : 1;
#endif
#else
            eliminate_candidate_based_on_pme_me_results = (enc_mode <= ENC_M7) ? 0 : 1;
#endif
#endif
#endif
     set_cand_elimination_controls(context_ptr,eliminate_candidate_based_on_pme_me_results);
#else
#if FTR_PD2_REDUCE_MDS0
     if (pd_pass == PD_PASS_0)
        context_ptr->eliminate_candidate_based_on_pme_me_results = 0;
    else if (pd_pass == PD_PASS_1)
        context_ptr->eliminate_candidate_based_on_pme_me_results = 0;
    else
        if (pcs_ptr->slice_type == I_SLICE)
            context_ptr->eliminate_candidate_based_on_pme_me_results = 0;
        else
#if TUNE_M4_M8
            context_ptr->eliminate_candidate_based_on_pme_me_results = (enc_mode <= ENC_M6) ? 0 : 1;
#else
            context_ptr->eliminate_candidate_based_on_pme_me_results = (enc_mode <= ENC_M7) ? 0 : 1;
#endif
#endif
#endif
#if FTR_REDUCE_TXT_BASED_ON_DISTORTION
     if (pd_pass == PD_PASS_0)
         context_ptr->bypass_tx_search_when_zcoef = 0;
     else if (pd_pass == PD_PASS_1)
         context_ptr->bypass_tx_search_when_zcoef = 0;
     else
         if (pcs_ptr->slice_type == I_SLICE)
             context_ptr->bypass_tx_search_when_zcoef = 0;
         else
#if TUNE_M4_M8
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_M7_M9
#if TUNE_M6_FEATURES
#if TUNE_M5_FEATURES
#if TUNE_SHIFT_PRESETS_DOWN && !TUNE_M0_M8_MEGA_FEB
             context_ptr->bypass_tx_search_when_zcoef = (enc_mode <= ENC_M3) ? 0 : 1;
#else
             context_ptr->bypass_tx_search_when_zcoef = (enc_mode <= ENC_M4) ? 0 : 1;
#endif
#else
             context_ptr->bypass_tx_search_when_zcoef = (enc_mode <= ENC_M5) ? 0 : 1;
#endif
#else
             context_ptr->bypass_tx_search_when_zcoef = (enc_mode <= ENC_M6) ? 0 : 1;
#endif
#else
             context_ptr->bypass_tx_search_when_zcoef = (enc_mode <= ENC_M7) ? 0 : 1;
#endif
#else
             context_ptr->bypass_tx_search_when_zcoef = (enc_mode <= ENC_M6) ? 0 : 1;
#endif
#else
             context_ptr->bypass_tx_search_when_zcoef = (enc_mode <= ENC_M7) ? 0 : 1;
#endif
#endif
#if TUNE_TXT_M9
#if TUNE_SHIFT_PRESETS_DOWN && !TUNE_M0_M8_MEGA_FEB
     if (enc_mode <= ENC_M7)
#else
     if (enc_mode <= ENC_M8)
#endif
         context_ptr->early_txt_search_exit_level = 0;
     else
         if(pcs_ptr->parent_pcs_ptr->input_resolution <= INPUT_SIZE_720p_RANGE)
             context_ptr->early_txt_search_exit_level = 1;
         else
             context_ptr->early_txt_search_exit_level = 2;
#else
#if OPT_TX_TYPE_SEARCH
     if (enc_mode <= ENC_M8)
         context_ptr->txt_exit_based_on_non_coeff_th = 0;
     else
#if TUNE_TXT_M9
         context_ptr->txt_exit_based_on_non_coeff_th = pcs_ptr->parent_pcs_ptr->input_resolution <= INPUT_SIZE_720p_RANGE ? 4 : 16;
#else
         context_ptr->txt_exit_based_on_non_coeff_th = pcs_ptr->parent_pcs_ptr->input_resolution <= INPUT_SIZE_720p_RANGE ? 8 : 32;
#endif
#endif
#endif
#if FTR_USE_SKIP_MD
#if TUNE_SHIFT_PRESETS_DOWN
     if (enc_mode <= ENC_M7)
#else
     if (enc_mode <= ENC_M8)
#endif
         context_ptr->ep_use_md_skip_decision = 0;
     else
         context_ptr->ep_use_md_skip_decision = 1;
#endif
#if OPT_LF
     if (enc_mode <= ENC_M8)
         context_ptr->sb_bypass_dlf = 0;
     else
         context_ptr->sb_bypass_dlf = 1;
#endif
#if OPT_LOSSLESS_1
     context_ptr->use_best_mds0 = 0;
     if (pd_pass == PD_PASS_0) {
         if (enc_mode <= ENC_M8)
             context_ptr->use_best_mds0 = 0;
         else
             context_ptr->use_best_mds0 = 1;
     }
#endif
    return return_error;
}
void copy_neighbour_arrays(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                           uint32_t src_idx, uint32_t dst_idx, uint32_t blk_mds, uint32_t sb_org_x,
                           uint32_t sb_org_y);

static void set_parent_to_be_considered(MdcSbData *results_ptr, uint32_t blk_index, int32_t sb_size,
                                        int8_t pred_depth,
                                        uint8_t pred_sq_idx,
#if OPT6_DEPTH_REFINEMENT
                                        const uint8_t disallow_nsq,
#endif
                                        int8_t depth_step) {
    const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);
    if (blk_geom->sq_size < ((sb_size == BLOCK_128X128) ? 128 : 64)) {
        //Set parent to be considered
        uint32_t parent_depth_idx_mds =
            (blk_geom->sqi_mds -
             (blk_geom->quadi - 3) * ns_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth]) -
            parent_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth];
        const BlockGeom *parent_blk_geom = get_blk_geom_mds(parent_depth_idx_mds);
#if OPT6_DEPTH_REFINEMENT
        const uint32_t parent_tot_d1_blocks = disallow_nsq ? 1 :
            parent_blk_geom->sq_size == 128 ? 17 :
            parent_blk_geom->sq_size > 8 ? 25 :
            parent_blk_geom->sq_size == 8 ? 5 : 1;
#else
        uint32_t         parent_tot_d1_blocks =
            parent_blk_geom->sq_size == 128
                ? 17
                : parent_blk_geom->sq_size > 8 ? 25 : parent_blk_geom->sq_size == 8 ? 5 : 1;
#endif
        for (uint32_t block_1d_idx = 0; block_1d_idx < parent_tot_d1_blocks; block_1d_idx++) {
#if !OPT_REFINEMENT_SIGNALS
            results_ptr->leaf_data_array[parent_depth_idx_mds + block_1d_idx].pred_depth_refinement = parent_blk_geom->depth - pred_depth;
            results_ptr->leaf_data_array[parent_depth_idx_mds + block_1d_idx].pred_depth = pred_sq_idx;
#endif
#if OPT6_DEPTH_REFINEMENT
            results_ptr->consider_block[parent_depth_idx_mds + block_1d_idx] = 1;
#else
            results_ptr->leaf_data_array[parent_depth_idx_mds + block_1d_idx].consider_block = 1;
#endif
        }

        if (depth_step < -1)
#if OPT6_DEPTH_REFINEMENT
            set_parent_to_be_considered(results_ptr, parent_depth_idx_mds, sb_size, pred_depth, pred_sq_idx, disallow_nsq ,depth_step + 1);
#else
            set_parent_to_be_considered(results_ptr, parent_depth_idx_mds, sb_size, pred_depth, pred_sq_idx, depth_step + 1);
#endif
    }
}
static void set_child_to_be_considered(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr, MdcSbData *results_ptr, uint32_t blk_index, uint32_t sb_index, int32_t sb_size,
    int8_t pred_depth,
    uint8_t pred_sq_idx,
    int8_t depth_step) {


    const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);
    unsigned         tot_d1_blocks = blk_geom->sq_size == 128
        ? 17
        : blk_geom->sq_size > 8 ? 25 : blk_geom->sq_size == 8 ? 5 : 1;
    if (blk_geom->sq_size > 4) {
        for (uint32_t block_1d_idx = 0; block_1d_idx < tot_d1_blocks; block_1d_idx++) {
#if OPT6_DEPTH_REFINEMENT
            results_ptr->consider_block[blk_index + block_1d_idx]= 1;
            results_ptr->refined_split_flag[blk_index + block_1d_idx] = EB_TRUE;
#else
            results_ptr->leaf_data_array[blk_index + block_1d_idx].consider_block     = 1;
            results_ptr->leaf_data_array[blk_index + block_1d_idx].refined_split_flag = EB_TRUE;
#endif
        }
        //Set first child to be considered
        uint32_t child_block_idx_1 = blk_index +
            d1_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth];
        const BlockGeom *child1_blk_geom = get_blk_geom_mds(child_block_idx_1);
#if OPT6_DEPTH_REFINEMENT
        const uint32_t child1_tot_d1_blocks = pcs_ptr->parent_pcs_ptr->disallow_nsq ? 1 :
            child1_blk_geom->sq_size == 128 ? 17:
            child1_blk_geom->sq_size > 8 ? 25 :
            child1_blk_geom->sq_size == 8 ? 5 :
            1;
#else
        uint32_t         child1_tot_d1_blocks =
            child1_blk_geom->sq_size == 128
                ? 17
                : child1_blk_geom->sq_size > 8 ? 25 : child1_blk_geom->sq_size == 8 ? 5 : 1;
#endif

        for (uint32_t block_1d_idx = 0; block_1d_idx < child1_tot_d1_blocks; block_1d_idx++) {
#if OPT6_DEPTH_REFINEMENT
            results_ptr->consider_block[child_block_idx_1 + block_1d_idx] = 1;
            results_ptr->refined_split_flag[child_block_idx_1 + block_1d_idx] = EB_FALSE;
#else
            results_ptr->leaf_data_array[child_block_idx_1 + block_1d_idx].consider_block = 1;
#if !OPT_REFINEMENT_SIGNALS
            results_ptr->leaf_data_array[child_block_idx_1 + block_1d_idx].pred_depth_refinement = child1_blk_geom->depth - pred_depth;
            results_ptr->leaf_data_array[child_block_idx_1 + block_1d_idx].pred_depth = pred_sq_idx;
#endif
            results_ptr->leaf_data_array[child_block_idx_1 + block_1d_idx].refined_split_flag =
                EB_FALSE;
#endif
        }
        // Add children blocks if more depth to consider (depth_step is > 1), or block not allowed (add next depth)
        if (depth_step > 1 || !pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_allowed[child_block_idx_1])
            set_child_to_be_considered(pcs_ptr, context_ptr, results_ptr, child_block_idx_1, sb_index, sb_size, pred_depth, pred_sq_idx , depth_step > 1 ? depth_step - 1 : 1);
        //Set second child to be considered
        uint32_t child_block_idx_2 = child_block_idx_1 +
            ns_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth + 1];
        const BlockGeom *child2_blk_geom = get_blk_geom_mds(child_block_idx_2);
#if OPT6_DEPTH_REFINEMENT
        const uint32_t child2_tot_d1_blocks = pcs_ptr->parent_pcs_ptr->disallow_nsq ? 1 :
            child2_blk_geom->sq_size == 128 ? 17:
            child2_blk_geom->sq_size > 8 ? 25 :
            child2_blk_geom->sq_size == 8 ? 5 :
            1;
#else
        uint32_t         child2_tot_d1_blocks =
            child2_blk_geom->sq_size == 128
                ? 17
                : child2_blk_geom->sq_size > 8 ? 25 : child2_blk_geom->sq_size == 8 ? 5 : 1;
#endif
        for (uint32_t block_1d_idx = 0; block_1d_idx < child2_tot_d1_blocks; block_1d_idx++) {
#if OPT6_DEPTH_REFINEMENT
            results_ptr->consider_block[child_block_idx_2 + block_1d_idx] = 1;
            results_ptr->refined_split_flag[child_block_idx_2 + block_1d_idx] = EB_FALSE;
#else
            results_ptr->leaf_data_array[child_block_idx_2 + block_1d_idx].consider_block = 1;
#if !OPT_REFINEMENT_SIGNALS
            results_ptr->leaf_data_array[child_block_idx_2 + block_1d_idx].pred_depth_refinement = child2_blk_geom->depth - pred_depth;
            results_ptr->leaf_data_array[child_block_idx_2 + block_1d_idx].pred_depth = pred_sq_idx;
#endif
            results_ptr->leaf_data_array[child_block_idx_2 + block_1d_idx].refined_split_flag =
                EB_FALSE;
#endif
        }
        // Add children blocks if more depth to consider (depth_step is > 1), or block not allowed (add next depth)
        if (depth_step > 1 || !pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_allowed[child_block_idx_2])
            set_child_to_be_considered(pcs_ptr, context_ptr, results_ptr, child_block_idx_2, sb_index, sb_size, pred_depth, pred_sq_idx , depth_step > 1 ? depth_step - 1 : 1);
        //Set third child to be considered
        uint32_t child_block_idx_3 = child_block_idx_2 +
            ns_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth + 1];
        const BlockGeom *child3_blk_geom = get_blk_geom_mds(child_block_idx_3);
#if OPT6_DEPTH_REFINEMENT
        const uint32_t child3_tot_d1_blocks = pcs_ptr->parent_pcs_ptr->disallow_nsq ? 1 :
            child3_blk_geom->sq_size == 128 ? 17:
            child3_blk_geom->sq_size > 8 ? 25 :
            child3_blk_geom->sq_size == 8 ? 5 :
            1;
#else
        uint32_t         child3_tot_d1_blocks =
            child3_blk_geom->sq_size == 128
                ? 17
                : child3_blk_geom->sq_size > 8 ? 25 : child3_blk_geom->sq_size == 8 ? 5 : 1;
#endif

        for (uint32_t block_1d_idx = 0; block_1d_idx < child3_tot_d1_blocks; block_1d_idx++) {
#if OPT6_DEPTH_REFINEMENT
            results_ptr->consider_block[child_block_idx_3 + block_1d_idx] = 1;
            results_ptr->refined_split_flag[child_block_idx_3 + block_1d_idx] = EB_FALSE;
#else
            results_ptr->leaf_data_array[child_block_idx_3 + block_1d_idx].consider_block = 1;
#if !OPT_REFINEMENT_SIGNALS
            results_ptr->leaf_data_array[child_block_idx_3 + block_1d_idx].pred_depth_refinement = child3_blk_geom->depth - pred_depth;
            results_ptr->leaf_data_array[child_block_idx_3 + block_1d_idx].pred_depth = pred_sq_idx;
#endif
            results_ptr->leaf_data_array[child_block_idx_3 + block_1d_idx].refined_split_flag =
                EB_FALSE;
#endif
        }

        // Add children blocks if more depth to consider (depth_step is > 1), or block not allowed (add next depth)
        if (depth_step > 1 || !pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_allowed[child_block_idx_3])
            set_child_to_be_considered(pcs_ptr, context_ptr, results_ptr, child_block_idx_3, sb_index, sb_size, pred_depth, pred_sq_idx , depth_step > 1 ? depth_step - 1 : 1);
        //Set forth child to be considered
        uint32_t child_block_idx_4 = child_block_idx_3 +
            ns_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth + 1];
        const BlockGeom *child4_blk_geom = get_blk_geom_mds(child_block_idx_4);
#if OPT6_DEPTH_REFINEMENT
        const uint32_t child4_tot_d1_blocks = pcs_ptr->parent_pcs_ptr->disallow_nsq ? 1 :
            child4_blk_geom->sq_size == 128 ? 17:
            child4_blk_geom->sq_size > 8 ? 25 :
            child4_blk_geom->sq_size == 8 ? 5 :
            1;
#else
        uint32_t         child4_tot_d1_blocks =
            child4_blk_geom->sq_size == 128
                ? 17
                : child4_blk_geom->sq_size > 8 ? 25 : child4_blk_geom->sq_size == 8 ? 5 : 1;
#endif
        for (uint32_t block_1d_idx = 0; block_1d_idx < child4_tot_d1_blocks; block_1d_idx++) {
#if OPT6_DEPTH_REFINEMENT
            results_ptr->consider_block[child_block_idx_4 + block_1d_idx] = 1;
            results_ptr->refined_split_flag[child_block_idx_4 + block_1d_idx] = EB_FALSE;
#else
            results_ptr->leaf_data_array[child_block_idx_4 + block_1d_idx].consider_block = 1;
#if !OPT_REFINEMENT_SIGNALS
            results_ptr->leaf_data_array[child_block_idx_4 + block_1d_idx].pred_depth_refinement = child4_blk_geom->depth - pred_depth;
            results_ptr->leaf_data_array[child_block_idx_4 + block_1d_idx].pred_depth = pred_sq_idx;
#endif
            results_ptr->leaf_data_array[child_block_idx_4 + block_1d_idx].refined_split_flag =
                EB_FALSE;
#endif
        }
        // Add children blocks if more depth to consider (depth_step is > 1), or block not allowed (add next depth)
        if (depth_step > 1 || !pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_allowed[child_block_idx_4])
            set_child_to_be_considered(pcs_ptr, context_ptr, results_ptr, child_block_idx_4, sb_index, sb_size, pred_depth, pred_sq_idx , depth_step > 1 ? depth_step - 1 : 1);
    }
}
#if !OPT_BUILD_CAND_BLK_2
void init_allowed_blocks(MdcSbData *results_ptr, ModeDecisionContext *context_ptr,
                                uint32_t blk_index, uint32_t tot_d1_blocks) {
    for (uint32_t d1_block_idx = 0; d1_block_idx < tot_d1_blocks; d1_block_idx++) {
        uint32_t d1_blk_idx                                        = blk_index + d1_block_idx;
        context_ptr->md_blk_arr_nsq[d1_blk_idx].split_flag         = EB_TRUE;
        context_ptr->md_local_blk_unit[d1_blk_idx].tested_blk_flag = EB_FALSE;
        context_ptr->md_blk_arr_nsq[d1_blk_idx].part               = PARTITION_SPLIT;
#if OPT6_DEPTH_REFINEMENT
        if (results_ptr->consider_block[d1_blk_idx]) {
#else
        if (results_ptr->leaf_data_array[d1_blk_idx].consider_block) {
#endif
            context_ptr->md_local_blk_unit[d1_blk_idx].left_neighbor_partition =
                INVALID_NEIGHBOR_DATA;
            context_ptr->md_local_blk_unit[d1_blk_idx].above_neighbor_partition =
                INVALID_NEIGHBOR_DATA;
#if CLN_NSQ_AND_STATS
            context_ptr->md_blk_arr_nsq[d1_blk_idx].do_not_process_block = 0;
#else
            if (!context_ptr->md_disallow_nsq)
                for (uint8_t shape_idx = 0; shape_idx < NUMBER_OF_SHAPES; shape_idx++)
                    context_ptr->md_local_blk_unit[d1_blk_idx].sse_gradian_band[shape_idx] = +1;
            context_ptr->md_blk_arr_nsq[d1_blk_idx].do_not_process_block = 0;
            AMdCycleRControls *adaptive_md_cycles_red_ctrls =
                &context_ptr->admd_cycles_red_ctrls;
            if (adaptive_md_cycles_red_ctrls->enabled) {
                if (adaptive_md_cycles_red_ctrls->skip_nsq_th) {
                    const BlockGeom *nsq_blk_geom = get_blk_geom_mds(d1_blk_idx);
                    if (nsq_blk_geom->shape != PART_N) {
                        int8_t pred_depth_refinement =
                            results_ptr->leaf_data_array[d1_blk_idx].pred_depth_refinement;
                        pred_depth_refinement = MIN(pred_depth_refinement, 2);
                        pred_depth_refinement = MAX(pred_depth_refinement, -2);
                        pred_depth_refinement += 2;
                        if (context_ptr->ad_md_prob[pred_depth_refinement][nsq_blk_geom->shape] <
                             adaptive_md_cycles_red_ctrls->skip_nsq_th)
                            results_ptr->leaf_data_array[d1_blk_idx].consider_block = 0;
                    }
                }
            }
#endif
        }
    }
}
#endif
#if OPT7_BUILD_CAND_BLK
#if OPT_BUILD_CAND_BLK_3
static INLINE uint32_t get_tot_1d_blks(struct PictureParentControlSet * ppcs, const int32_t sq_size, const uint8_t disallow_nsq) {
#else
INLINE uint32_t get_tot_1d_blks(struct PictureParentControlSet * ppcs, const int32_t sq_size, const uint8_t disallow_nsq) {
#endif
    uint32_t tot_d1_blocks;

    tot_d1_blocks = (disallow_nsq) ||
        (sq_size >= 64 && ppcs->disallow_all_nsq_blocks_above_64x64) ||
        (sq_size >= 32 && ppcs->disallow_all_nsq_blocks_above_32x32) ||
        (sq_size >= 16 && ppcs->disallow_all_nsq_blocks_above_16x16) ||
        (sq_size <= 64 && ppcs->disallow_all_nsq_blocks_below_64x64) ||
        (sq_size <= 32 && ppcs->disallow_all_nsq_blocks_below_32x32) ||
        (sq_size <= 8 && ppcs->disallow_all_nsq_blocks_below_8x8) ||
        (sq_size <= 16 && ppcs->disallow_all_nsq_blocks_below_16x16) ? 1 :
        (sq_size == 16 && ppcs->disallow_all_non_hv_nsq_blocks_below_16x16) ? 5 :
        (sq_size == 16 && ppcs->disallow_all_h4_v4_blocks_below_16x16) ? 17 :
        sq_size == 128
        ? 17
        : sq_size > 8 ? 25 : sq_size == 8 ? 5 : 1;

    if (ppcs->disallow_HVA_HVB_HV4)
        tot_d1_blocks = MIN(5, tot_d1_blocks);

    if (ppcs->disallow_HV4)
        tot_d1_blocks = MIN(17, tot_d1_blocks);

    return tot_d1_blocks;
}
#endif
#if OPT_BUILD_CAND_BLK_3
// Initialize structures used to indicate which blocks will be tested at MD.
// MD data structures should be updated in init_block_data(), not here.
static void build_cand_block_array(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
    uint32_t sb_index, EbBool is_complete_sb) {

    memset(context_ptr->tested_blk_flag, 0, sizeof(uint8_t) * scs_ptr->max_block_cnt);
    memset(context_ptr->do_not_process_blk, 0, sizeof(uint8_t) * scs_ptr->max_block_cnt);

    MdcSbData *results_ptr = context_ptr->mdc_sb_array;
    results_ptr->leaf_count = 0;
    uint32_t blk_index = 0;
#if !LIGHT_PD0
    const BlockSize sb_size = scs_ptr->seq_header.sb_size;
#endif
    const uint16_t max_block_cnt = scs_ptr->max_block_cnt;
    int32_t min_sq_size;
    //if (context_ptr->pred_depth_only)
    //    min_sq_size = (context_ptr->depth_removal_ctrls.enabled && context_ptr->depth_removal_ctrls.disallow_below_64x64)
    //    ? 64
    //    : (context_ptr->depth_removal_ctrls.enabled && context_ptr->depth_removal_ctrls.disallow_below_32x32)
    //    ? 32
    //    : (context_ptr->depth_removal_ctrls.enabled && context_ptr->depth_removal_ctrls.disallow_below_16x16)
    //    ? 16
    //    : context_ptr->disallow_4x4 ? 8 : 4;
    //else
        min_sq_size = context_ptr->disallow_4x4 ? 8 : 4;

    while (blk_index < max_block_cnt) {
        const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);

        // Initialize here because may not be updated at inter-depth decision for incomplete SBs
        if (!is_complete_sb)
            context_ptr->md_blk_arr_nsq[blk_index].part = PARTITION_SPLIT;

        // SQ/NSQ block(s) filter based on the SQ size
        uint8_t is_block_tagged =
            (blk_geom->sq_size == 128 && pcs_ptr->slice_type == I_SLICE) /*||
            (context_ptr->pred_depth_only && (blk_geom->sq_size < min_sq_size))*/
            ? 0
            : 1;

        // SQ/NSQ block(s) filter based on the block validity
        if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index] && is_block_tagged) {

            const uint32_t tot_d1_blocks = pcs_ptr->parent_pcs_ptr->disallow_nsq ? 1 :
                get_tot_1d_blks(pcs_ptr->parent_pcs_ptr, blk_geom->sq_size, context_ptr->md_disallow_nsq);

            for (uint32_t idx = blk_index; idx < (tot_d1_blocks + blk_index); ++idx) {

                if (results_ptr->consider_block[idx]) {
                    results_ptr->leaf_data_array[results_ptr->leaf_count].mds_idx = idx;
                    results_ptr->leaf_data_array[results_ptr->leaf_count].tot_d1_blocks = tot_d1_blocks;
                    results_ptr->split_flag[results_ptr->leaf_count++] = results_ptr->refined_split_flag[idx];
                }
            }
#if LIGHT_PD0
            blk_index += blk_geom->d1_depth_offset;
#else
            blk_index += d1_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth];
#endif
        }
        else {
#if LIGHT_PD0
            blk_index +=
                (blk_geom->sq_size > min_sq_size)
                ? blk_geom->d1_depth_offset
                : blk_geom->ns_depth_offset;
#else
            blk_index +=
                (blk_geom->sq_size > min_sq_size)
                ? d1_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth]
                : ns_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth];
#endif
        }
    }
}
#else
static void build_cand_block_array(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
    uint32_t sb_index) {

#if OPT_BUILD_CAND_BLK_2
    memset(context_ptr->tested_blk_flag, 0, sizeof(uint8_t) * scs_ptr->max_block_cnt);
    memset(context_ptr->do_not_process_blk, 0, sizeof(uint8_t) * scs_ptr->max_block_cnt);
#endif
    MdcSbData *results_ptr = context_ptr->mdc_sb_array;
    results_ptr->leaf_count = 0;
    uint32_t blk_index = 0;
#if !OPT_BUILD_CAND_BLK_2
    uint32_t d1_blocks_accumlated, d1_block_idx;
#endif
    while (blk_index < scs_ptr->max_block_cnt) {
        const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);

        // SQ/NSQ block(s) filter based on the SQ size
        uint8_t is_block_tagged =
            (blk_geom->sq_size == 128 && pcs_ptr->slice_type == I_SLICE)
            ? 0
            : 1;

        // split_flag is f(min_sq_size)
        int32_t min_sq_size = (context_ptr->disallow_4x4) ? 8 : 4;

        // SQ/NSQ block(s) filter based on the block validity
        if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index] && is_block_tagged) {
#if OPT7_BUILD_CAND_BLK
            uint32_t tot_d1_blocks = pcs_ptr->parent_pcs_ptr->disallow_nsq ? 1:
                (context_ptr->md_disallow_nsq) ||
#else
            uint32_t tot_d1_blocks = (context_ptr->md_disallow_nsq) ||
#endif
                (blk_geom->sq_size >= 64 && pcs_ptr->parent_pcs_ptr->disallow_all_nsq_blocks_above_64x64) ||
                (blk_geom->sq_size >= 32 && pcs_ptr->parent_pcs_ptr->disallow_all_nsq_blocks_above_32x32) ||
                (blk_geom->sq_size >= 16 && pcs_ptr->parent_pcs_ptr->disallow_all_nsq_blocks_above_16x16) ||
                (blk_geom->sq_size <= 64 && pcs_ptr->parent_pcs_ptr->disallow_all_nsq_blocks_below_64x64) ||
                (blk_geom->sq_size <= 32 && pcs_ptr->parent_pcs_ptr->disallow_all_nsq_blocks_below_32x32) ||
                (blk_geom->sq_size <= 8 && pcs_ptr->parent_pcs_ptr->disallow_all_nsq_blocks_below_8x8) ||
                (blk_geom->sq_size <= 16 && pcs_ptr->parent_pcs_ptr->disallow_all_nsq_blocks_below_16x16) ? 1 :
                (blk_geom->sq_size == 16 && pcs_ptr->parent_pcs_ptr->disallow_all_non_hv_nsq_blocks_below_16x16) ? 5 :
                (blk_geom->sq_size == 16 && pcs_ptr->parent_pcs_ptr->disallow_all_h4_v4_blocks_below_16x16) ? 17 :
                blk_geom->sq_size == 128
                ? 17
                : blk_geom->sq_size > 8 ? 25 : blk_geom->sq_size == 8 ? 5 : 1;

            if (pcs_ptr->parent_pcs_ptr->disallow_HVA_HVB_HV4)
                tot_d1_blocks = MIN(5, tot_d1_blocks);

            if (pcs_ptr->parent_pcs_ptr->disallow_HV4)
                tot_d1_blocks = MIN(17, tot_d1_blocks);
#if !OPT_BUILD_CAND_BLK_2
            d1_blocks_accumlated = 0;
            init_allowed_blocks(results_ptr, context_ptr, blk_index, tot_d1_blocks);

            for (d1_block_idx = 0; d1_block_idx < tot_d1_blocks; d1_block_idx++)
#if OPT6_DEPTH_REFINEMENT
                d1_blocks_accumlated += results_ptr->consider_block[blk_index + d1_block_idx] ? 1 : 0;
#else
                d1_blocks_accumlated +=
                results_ptr->leaf_data_array[blk_index + d1_block_idx].consider_block ? 1 : 0;
#endif
#endif
            for (uint32_t idx = 0; idx < tot_d1_blocks; ++idx) {
#if OPT_BUILD_CAND_BLK_2
                context_ptr->md_blk_arr_nsq[blk_index].part = PARTITION_SPLIT;
#endif
#if OPT6_DEPTH_REFINEMENT
                if (results_ptr->consider_block[blk_index]) {
#else
                if (results_ptr->leaf_data_array[blk_index].consider_block) {
#endif
                    results_ptr->leaf_data_array[results_ptr->leaf_count].mds_idx = blk_index;
#if OPT_BUILD_CAND_BLK_2
                    results_ptr->leaf_data_array[results_ptr->leaf_count].tot_d1_blocks = tot_d1_blocks;
#else
                    results_ptr->leaf_data_array[results_ptr->leaf_count].tot_d1_blocks = d1_blocks_accumlated;
#endif
#if !OPT_REFINEMENT_SIGNALS
                    results_ptr->leaf_data_array[results_ptr->leaf_count].final_pred_depth_refinement = results_ptr->leaf_data_array[blk_index].pred_depth_refinement;
                    if (results_ptr->leaf_data_array[results_ptr->leaf_count].final_pred_depth_refinement == -8)
                        printf("final_pred_depth_refinement error\n");
                    results_ptr->leaf_data_array[results_ptr->leaf_count].final_pred_depth = results_ptr->leaf_data_array[blk_index].pred_depth;
                    if (results_ptr->leaf_data_array[results_ptr->leaf_count].final_pred_depth == -8)
                        printf("final_pred_depth error\n");
#endif
#if OPT6_DEPTH_REFINEMENT
                    results_ptr->split_flag[results_ptr->leaf_count++] = results_ptr->refined_split_flag[blk_index];
#else

                    results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag = results_ptr->leaf_data_array[blk_index].refined_split_flag;
#endif

                }
                blk_index++;
            }
            blk_index +=
                (d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth] -
                    tot_d1_blocks);
        }
        else {
            blk_index +=
                (blk_geom->sq_size > min_sq_size)
                ? d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth]
                : ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
        }
    }
}
#endif
#if !TUNE_REMOVE_TXT_STATS
void generate_statistics_txt(
    SequenceControlSet  *scs_ptr,
    PictureControlSet   *pcs_ptr,
    ModeDecisionContext *context_ptr,
    uint32_t             sb_index) {
    uint32_t blk_index = 0;
    uint32_t part_cnt[TXT_DEPTH_DELTA_NUM][TX_TYPES] = { {0},{0} };
    while (blk_index < scs_ptr->max_block_cnt) {
        const BlockGeom * blk_geom = get_blk_geom_mds(blk_index);
        uint8_t is_blk_allowed = pcs_ptr->slice_type != I_SLICE ? 1 :
            (blk_geom->sq_size < 128) ? 1 : 0;
        EbBool split_flag = context_ptr->md_blk_arr_nsq[blk_index].split_flag;
        if (scs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index] && is_blk_allowed) {
            if (blk_geom->shape == PART_N) {
                if (context_ptr->md_blk_arr_nsq[blk_index].split_flag == EB_FALSE) {
                    if (context_ptr->avail_blk_flag[blk_index]) {
                        uint8_t part_idx = context_ptr->md_blk_arr_nsq[blk_index].part;
                        int8_t pred_depth_refinement = context_ptr->md_local_blk_unit[blk_geom->sqi_mds].pred_depth_refinement;
                        // Set the bounds of pred_depth_refinement for array indexing
                        pred_depth_refinement = MIN(pred_depth_refinement, 1);
                        pred_depth_refinement = MAX(pred_depth_refinement, -1);
                        // Add one b/c starts at -1 (need proper offset for array)
                        // Only track whether the refinement is positive or negative
                        pred_depth_refinement++;
                        if (pred_depth_refinement < 0 || pred_depth_refinement >(TXT_DEPTH_DELTA_NUM - 1))
                            printf("pred_depth_refinement array idx error\t%d\n", pred_depth_refinement);
                        // Select the best partition, blk_index refers to the SQ block
                        uint32_t best_idx = 0;
                        uint32_t blks_in_best = 0;
                        switch (part_idx) {
                        case PARTITION_NONE:
                            best_idx = blk_index;
                            blks_in_best = 1;
                            break;
                        case PARTITION_HORZ:
                            best_idx = blk_index + 1;
                            blks_in_best = 2;
                            break;
                        case PARTITION_VERT:
                            best_idx = blk_index + 3;
                            blks_in_best = 2;
                            break;
                        case PARTITION_HORZ_A:
                            best_idx = blk_index + 5;
                            blks_in_best = 3;
                            break;
                        case PARTITION_HORZ_B:
                            best_idx = blk_index + 8;
                            blks_in_best = 3;
                            break;
                        case PARTITION_VERT_A:
                            best_idx = blk_index + 11;
                            blks_in_best = 3;
                            break;
                        case PARTITION_VERT_B:
                            best_idx = blk_index + 14;
                            blks_in_best = 3;
                            break;
                        case PARTITION_HORZ_4:
                            best_idx = blk_index + 17;
                            blks_in_best = 4;
                            break;
                        case PARTITION_VERT_4:
                            best_idx = blk_index + 21;
                            blks_in_best = 4;
                            break;
                        default:
                            assert(0);
                            break;
                        }
                        // Loop over the blocks in the best partition
                        for (uint32_t curr_idx = best_idx; curr_idx < best_idx + blks_in_best; curr_idx++) {
                            // Use the info of the best partition, not the square (only partition, cost,
                            // and split_flag are updated in the SQ block as the best
                            const BlockGeom * best_blk_geom = get_blk_geom_mds(curr_idx);
                            uint8_t tx_depth = context_ptr->md_blk_arr_nsq[curr_idx].tx_depth;
                            for (uint8_t txb_itr = 0; txb_itr < best_blk_geom->txb_count[tx_depth]; txb_itr++) {
                                uint8_t tx_type = context_ptr->md_blk_arr_nsq[curr_idx].txb_array[txb_itr].transform_type[PLANE_TYPE_Y];
                                uint32_t count_unit = (best_blk_geom->tx_width[tx_depth][txb_itr] * best_blk_geom->tx_height[tx_depth][txb_itr]); // count the area, not just the occurence
                                part_cnt[pred_depth_refinement][tx_type] += count_unit;
                            }
                        }
                    }
                }
            }
        }
        blk_index += split_flag ?
            d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth] :
            ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
    }
    for (uint8_t depth_delta = 0; depth_delta < TXT_DEPTH_DELTA_NUM; depth_delta++)
        for (uint8_t txs_idx = 0; txs_idx < TX_TYPES; txs_idx++)
            context_ptr->txt_cnt[depth_delta][txs_idx] += part_cnt[depth_delta][txs_idx];
}
#endif
#if !CLN_NSQ_AND_STATS
Part part_to_shape[NUMBER_OF_SHAPES] = {
    PART_N,
    PART_H,
    PART_V,
    PART_S,
    PART_HA,
    PART_HB,
    PART_VA,
    PART_VB,
    PART_H4,
    PART_V4
};
void generate_statistics_depth(
    SequenceControlSet  *scs_ptr,
    PictureControlSet   *pcs_ptr,
    ModeDecisionContext *context_ptr,
    uint32_t             sb_index) {
    uint32_t blk_index = 0;
    // init stat
    while (blk_index < scs_ptr->max_block_cnt) {
        const BlockGeom * blk_geom = get_blk_geom_mds(blk_index);
        uint8_t is_blk_allowed = pcs_ptr->slice_type != I_SLICE ? 1 :
            (blk_geom->sq_size < 128) ? 1 : 0;
        EbBool split_flag = context_ptr->md_blk_arr_nsq[blk_index].split_flag;
        if (scs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index] &&
            is_blk_allowed) {
            if (blk_geom->shape == PART_N) {
                if (context_ptr->md_blk_arr_nsq[blk_index].split_flag == EB_FALSE) {
                    if (context_ptr->avail_blk_flag[blk_index]) {
                        int8_t pred_depth_refinement = context_ptr->md_local_blk_unit[blk_geom->sqi_mds].pred_depth_refinement;
                        pred_depth_refinement = MIN(pred_depth_refinement, 1);
                        pred_depth_refinement = MAX(pred_depth_refinement, -1);
                        uint8_t part_idx = part_to_shape[context_ptr->md_blk_arr_nsq[blk_index].part];
                        context_ptr->pred_depth_count[pred_depth_refinement + 2][part_idx]+= (blk_geom->bwidth*blk_geom->bheight);
                    }
                }
            }
        }
        blk_index += split_flag ?
            d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth] :
            ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
    }
}
/******************************************************
* Generate probabilities for the depth_cycles_reduction
******************************************************/
void generate_depth_prob(PictureControlSet * pcs_ptr, ModeDecisionContext *context_ptr)
{
    if (pcs_ptr->parent_pcs_ptr->slice_type != I_SLICE) {
        uint32_t pred_depth_count[DEPTH_DELTA_NUM][NUMBER_OF_SHAPES - 1] = { {0},{0},{0},{0},{0} };
        uint32_t samples_num = 0;
        // Sum statistics from reference list0
        for (uint8_t ref_idx = 0; ref_idx < pcs_ptr->parent_pcs_ptr->ref_list0_count_try; ref_idx++) {
            EbReferenceObject *ref_obj_l0 =
                (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_0][ref_idx]->object_ptr;
            for (uint8_t pred_depth = 0; pred_depth < DEPTH_DELTA_NUM; pred_depth++) {
                for (uint8_t part_idx = 0; part_idx < (NUMBER_OF_SHAPES - 1); part_idx++) {
                    pred_depth_count[pred_depth][part_idx] += ref_obj_l0->ref_pred_depth_count[pred_depth][part_idx];
                    samples_num += ref_obj_l0->ref_pred_depth_count[pred_depth][part_idx];
                }
            }
        }
        // Sum statistics from reference list1
        for (uint8_t ref_idx = 0; ref_idx < pcs_ptr->parent_pcs_ptr->ref_list1_count_try; ref_idx++) {
            EbReferenceObject *ref_obj_l1 =
                (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_1][ref_idx]->object_ptr;
            for (uint8_t pred_depth = 0; pred_depth < DEPTH_DELTA_NUM; pred_depth++) {
                for (uint8_t part_idx = 0; part_idx < (NUMBER_OF_SHAPES - 1); part_idx++) {
                    pred_depth_count[pred_depth][part_idx] += ref_obj_l1->ref_pred_depth_count[pred_depth][part_idx];
                    samples_num += ref_obj_l1->ref_pred_depth_count[pred_depth][part_idx];
                }
            }
        }
        // Generate the selection %
        assert(samples_num > 0);
        for (uint8_t pred_depth = 0; pred_depth < DEPTH_DELTA_NUM; pred_depth++) {
            for (uint8_t part_idx = 0; part_idx < (NUMBER_OF_SHAPES - 1); part_idx++) {
                context_ptr->ad_md_prob[pred_depth][part_idx] = (uint32_t)((pred_depth_count[pred_depth][part_idx] * (uint32_t)DEPTH_PROB_PRECISION) / (uint32_t)samples_num);
            }
        }
        //Generate depth prob
        for (uint8_t pred_depth = 0; pred_depth < DEPTH_DELTA_NUM; pred_depth++) {
            for (uint8_t part_idx = 1; part_idx < (NUMBER_OF_SHAPES - 1); part_idx++) {
                pred_depth_count[pred_depth][0] += pred_depth_count[pred_depth][part_idx];
            }
        }
        for (uint8_t pred_depth = 0; pred_depth < DEPTH_DELTA_NUM; pred_depth++) {
            context_ptr->depth_prob[pred_depth] = (uint32_t)((pred_depth_count[pred_depth][0] * (uint32_t)100) / (uint32_t)samples_num);
        }

    }
    else {
        memcpy(context_ptr->ad_md_prob, intra_adaptive_md_cycles_reduction_th, sizeof(uint32_t) * DEPTH_DELTA_NUM * (NUMBER_OF_SHAPES - 1));
    }
}
/******************************************************
* Generate probabilities for the nsq_cycles_reduction
******************************************************/
#endif
#if !CLN_REMOVE_UNUSED_CODE
void generate_nsq_prob(PictureControlSet * pcs_ptr,ModeDecisionContext *context_ptr)
{
    if (pcs_ptr->parent_pcs_ptr->slice_type != I_SLICE) {
        uint32_t part_cnt[NUMBER_OF_SHAPES-1][FB_NUM][SSEG_NUM];
        uint8_t band, partidx, sse_idx;
        uint64_t samples_num = 0;
        // init stat
        memset(part_cnt, 0, sizeof(uint32_t) * (NUMBER_OF_SHAPES-1) * FB_NUM * SSEG_NUM);
        // Sum statistics from reference list0
        for (uint8_t ref_idx = 0; ref_idx < pcs_ptr->parent_pcs_ptr->ref_list0_count_try; ref_idx++) {
            EbReferenceObject *ref_obj_l0 =
                (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_0][ref_idx]->object_ptr;
            for (partidx = 0; partidx < NUMBER_OF_SHAPES-1; partidx++) {
                for (band = 0; band < FB_NUM; band++) {
                    for (sse_idx = 0; sse_idx < SSEG_NUM; sse_idx++) {
                        part_cnt[partidx][band][sse_idx] += ref_obj_l0->ref_part_cnt[partidx][band][sse_idx];
                        samples_num += ref_obj_l0->ref_part_cnt[partidx][band][sse_idx];
                    }
                }
            }
        }
        // Sum statistics from reference list1
        for (uint8_t ref_idx = 0; ref_idx < pcs_ptr->parent_pcs_ptr->ref_list1_count_try; ref_idx++) {
            EbReferenceObject *ref_obj_l1 =
                (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_1][ref_idx]->object_ptr;
            for (partidx = 0; partidx < NUMBER_OF_SHAPES-1; partidx++) {
                for (band = 0; band < FB_NUM; band++) {
                    for (sse_idx = 0; sse_idx < SSEG_NUM; sse_idx++) {
                        part_cnt[partidx][band][sse_idx] += ref_obj_l1->ref_part_cnt[partidx][band][sse_idx];
                        samples_num += ref_obj_l1->ref_part_cnt[partidx][band][sse_idx];
                    }
                }
            }
        }
        for (partidx = 0; partidx < NUMBER_OF_SHAPES-1; partidx++) {
            for (band = 0; band < FB_NUM; band++) {
                for (sse_idx = 0; sse_idx < SSEG_NUM; sse_idx++) {
                    context_ptr->part_prob[partidx][band][sse_idx] = samples_num ? (uint16_t)((part_cnt[partidx][band][sse_idx] * 1000) / samples_num) :
                        block_prob_tab[0][partidx][band][sse_idx];
                }
            }
        }
    }
}
void generate_statistics_nsq(
    SequenceControlSet  *scs_ptr,
    PictureControlSet   *pcs_ptr,
    ModeDecisionContext *context_ptr,
    uint32_t             sb_index) {
    uint32_t blk_index = 0;
    uint32_t part_cnt[NUMBER_OF_SHAPES-1][FB_NUM][SSEG_NUM];
    for (uint8_t partidx = 0; partidx < NUMBER_OF_SHAPES-1; partidx++) {
        for (uint8_t band = 0; band < FB_NUM; band++) {
            memset(part_cnt[partidx][band], 0, sizeof(uint32_t) * SSEG_NUM);
        }
    }
    while (blk_index < scs_ptr->max_block_cnt) {
        const BlockGeom * blk_geom = get_blk_geom_mds(blk_index);
        uint8_t is_blk_allowed = pcs_ptr->slice_type != I_SLICE ? 1 :
            (blk_geom->sq_size < 128) ? 1 : 0;
        EbBool split_flag = context_ptr->md_blk_arr_nsq[blk_index].split_flag;
        if (scs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index] &&
            is_blk_allowed) {
            if (blk_geom->shape == PART_N) {
                if (context_ptr->md_blk_arr_nsq[blk_index].split_flag == EB_FALSE) {
                    if (context_ptr->avail_blk_flag[blk_index]) {
                        uint8_t band_idx = 0;
                        uint8_t sq_size_idx = 7 - (uint8_t)svt_log2f((uint8_t)blk_geom->sq_size);
                        uint64_t band_width = (sq_size_idx == 0) ? 100 : (sq_size_idx == 1) ? 50 : 20;
                        uint8_t part_idx = part_to_shape[context_ptr->md_blk_arr_nsq[blk_index].part];

                        uint8_t sse_g_band = (!context_ptr->md_disallow_nsq && context_ptr->avail_blk_flag[blk_geom->sqi_mds]) ?
                            context_ptr->md_local_blk_unit[blk_geom->sqi_mds].sse_gradian_band[part_idx] : 1;
                        const uint32_t count_non_zero_coeffs = context_ptr->md_local_blk_unit[blk_index].count_non_zero_coeffs;
                        const uint32_t total_samples = (blk_geom->bwidth*blk_geom->bheight);
                        if (count_non_zero_coeffs >= ((total_samples * 18) / band_width)) {
                            band_idx = 9;
                        }
                        else if (count_non_zero_coeffs >= ((total_samples * 16) / band_width)) {
                            band_idx = 8;
                        }
                        else if (count_non_zero_coeffs >= ((total_samples * 14) / band_width)) {
                            band_idx = 7;
                        }
                        else if (count_non_zero_coeffs >= ((total_samples * 12) / band_width)) {
                            band_idx = 6;
                        }
                        else if (count_non_zero_coeffs >= ((total_samples * 10) / band_width)) {
                            band_idx = 5;
                        }
                        else if (count_non_zero_coeffs >= ((total_samples * 8) / band_width)) {
                            band_idx = 4;
                        }
                        else if (count_non_zero_coeffs >= ((total_samples * 6) / band_width)) {
                            band_idx = 3;
                        }
                        else if (count_non_zero_coeffs >= ((total_samples * 4) / band_width)) {
                            band_idx = 2;
                        }
                        else if (count_non_zero_coeffs >= ((total_samples * 2) / band_width)) {
                            band_idx = 1;
                        }
                        else {
                            band_idx = 0;
                        }
                        if (sq_size_idx == 0)
                            band_idx = band_idx == 0 ? 0 : band_idx <= 2 ? 1 : 2;
                        else if (sq_size_idx == 1)
                            band_idx = band_idx == 0 ? 0 : band_idx <= 3 ? 1 : 2;
                        else
                            band_idx = band_idx == 0 ? 0 : band_idx <= 8 ? 1 : 2;

                        part_cnt[part_to_shape[context_ptr->md_blk_arr_nsq[blk_index].part]][band_idx][sse_g_band] += (blk_geom->bwidth*blk_geom->bheight);

                    }
                }
            }
        }
        blk_index += split_flag ?
            d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth] :
            ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
    }
    for (uint8_t partidx = 0; partidx < NUMBER_OF_SHAPES-1; partidx++) {
        for (uint8_t band = 0; band < FB_NUM; band++) {
            for (uint8_t sse_idx = 0; sse_idx < SSEG_NUM; sse_idx++) {
                context_ptr->part_cnt[partidx][band][sse_idx] += part_cnt[partidx][band][sse_idx];
            }
        }
    }
}
#endif
#if !TUNE_REMOVE_TXT_STATS
/******************************************************
* Generate probabilities for the txt_cycles_reduction
******************************************************/
void generate_txt_prob(PictureControlSet * pcs_ptr,ModeDecisionContext *context_ptr)
{
    if (pcs_ptr->parent_pcs_ptr->slice_type != I_SLICE) {
        uint32_t txt_cnt[TXT_DEPTH_DELTA_NUM][TX_TYPES] = { {0},{0} };
        uint32_t samples_num = 0;
        // Sum statistics from reference list0
        for (uint8_t ref_idx = 0; ref_idx < pcs_ptr->parent_pcs_ptr->ref_list0_count_try; ref_idx++) {
            EbReferenceObject *ref_obj_l0 =
                (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_0][ref_idx]->object_ptr;
            for (uint8_t depth_delta = 0; depth_delta < TXT_DEPTH_DELTA_NUM; depth_delta++) {
                for (uint8_t txs_idx = 0; txs_idx < TX_TYPES; txs_idx++) {
                    txt_cnt[depth_delta][txs_idx] += ref_obj_l0->ref_txt_cnt[depth_delta][txs_idx];
                    samples_num += ref_obj_l0->ref_txt_cnt[depth_delta][txs_idx];
                }
            }
        }
        // Sum statistics from reference list1
        for (uint8_t ref_idx = 0; ref_idx < pcs_ptr->parent_pcs_ptr->ref_list1_count_try; ref_idx++) {
            EbReferenceObject *ref_obj_l1 =
                (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_1][ref_idx]->object_ptr;
            for (uint8_t depth_delta = 0; depth_delta < TXT_DEPTH_DELTA_NUM; depth_delta++) {
                for (uint8_t txs_idx = 1; txs_idx < TX_TYPES; txs_idx++) {
                    txt_cnt[depth_delta][txs_idx] += ref_obj_l1->ref_txt_cnt[depth_delta][txs_idx];
                    samples_num += ref_obj_l1->ref_txt_cnt[depth_delta][txs_idx];
                }
            }
        }
        assert(samples_num > 0);
        for (uint8_t depth_delta = 0; depth_delta < TXT_DEPTH_DELTA_NUM; depth_delta++) {
            for (uint8_t txs_idx = 1; txs_idx < TX_TYPES; txs_idx++) {
                context_ptr->txt_prob[depth_delta][txs_idx] = (uint32_t)((txt_cnt[depth_delta][txs_idx] * (uint32_t)10000) / (uint32_t)samples_num);
            }
        }
    }
}
#endif
#if !OPT_SB_CLASS
const uint32_t sb_class_th[NUMBER_OF_SB_CLASS] = { 0,85,75,65,60,55,50,45,40,
                                                   35,30,25,20,17,14,10,6,3,0 };
static uint8_t determine_sb_class(
    SequenceControlSet  *scs_ptr,
    PictureControlSet   *pcs_ptr,
    ModeDecisionContext *context_ptr,
    uint32_t             sb_index) {
    uint32_t blk_index = 0;
    uint64_t total_samples = 0;
    uint64_t count_non_zero_coeffs = 0;
    uint8_t sb_class = NONE_CLASS;
    while (blk_index < scs_ptr->max_block_cnt) {
        const BlockGeom * blk_geom = get_blk_geom_mds(blk_index);
        uint8_t is_blk_allowed = pcs_ptr->slice_type != I_SLICE ? 1 :
            (blk_geom->sq_size < 128) ? 1 : 0;
        EbBool split_flag = context_ptr->md_blk_arr_nsq[blk_index].split_flag;
        if (scs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index] &&
            context_ptr->avail_blk_flag[blk_index] &&
            is_blk_allowed) {
            if (blk_geom->shape == PART_N) {
                if (context_ptr->md_blk_arr_nsq[blk_index].split_flag == EB_FALSE) {
                    count_non_zero_coeffs += context_ptr->md_local_blk_unit[blk_index].count_non_zero_coeffs;
                    total_samples += (blk_geom->bwidth*blk_geom->bheight);
                }
            }
        }
        blk_index += split_flag ?
            d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth] :
            ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
    }
    if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_1]) / 100))
        sb_class = SB_CLASS_1;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_2]) / 100))
        sb_class = SB_CLASS_2;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_3]) / 100))
        sb_class = SB_CLASS_3;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_4]) / 100))
        sb_class = SB_CLASS_4;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_5]) / 100))
        sb_class = SB_CLASS_5;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_6]) / 100))
        sb_class = SB_CLASS_6;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_7]) / 100))
        sb_class = SB_CLASS_7;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_8]) / 100))
        sb_class = SB_CLASS_8;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_9]) / 100))
        sb_class = SB_CLASS_9;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_10]) / 100))
        sb_class = SB_CLASS_10;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_11]) / 100))
        sb_class = SB_CLASS_11;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_12]) / 100))
        sb_class = SB_CLASS_12;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_13]) / 100))
        sb_class = SB_CLASS_13;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_14]) / 100))
        sb_class = SB_CLASS_14;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_15]) / 100))
        sb_class = SB_CLASS_15;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_16]) / 100))
        sb_class = SB_CLASS_16;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_17]) / 100))
        sb_class = SB_CLASS_17;
    else if (count_non_zero_coeffs >= ((total_samples * sb_class_th[SB_CLASS_18]) / 100))
        sb_class = SB_CLASS_18;
    return sb_class;
}
#endif
void update_pred_th_offset(ModeDecisionContext *mdctxt, const BlockGeom *blk_geom, int8_t *s_depth, int8_t *e_depth, int64_t *th_offset) {

    uint32_t full_lambda = mdctxt->hbd_mode_decision ?
        mdctxt->full_lambda_md[EB_10_BIT_MD] :
        mdctxt->full_lambda_md[EB_8_BIT_MD];

#if TUNE_IMPROVE_DEPTH_REFINEMENT
    uint64_t cost_th_0 = (RDCOST(full_lambda, 16, 200 * blk_geom->bwidth * blk_geom->bheight) << (mdctxt->depth_refinement_ctrls.use_pred_block_cost - 1));
    uint64_t cost_th_1 = (RDCOST(full_lambda, 16, 300 * blk_geom->bwidth * blk_geom->bheight) << (mdctxt->depth_refinement_ctrls.use_pred_block_cost - 1));
    uint64_t cost_th_2 = (RDCOST(full_lambda, 16, 400 * blk_geom->bwidth * blk_geom->bheight) << (mdctxt->depth_refinement_ctrls.use_pred_block_cost - 1));
#else
    uint64_t cost_th_0 = RDCOST(full_lambda, 16, 200 * blk_geom->bwidth * blk_geom->bheight);
    uint64_t cost_th_1 = RDCOST(full_lambda, 16, 300 * blk_geom->bwidth * blk_geom->bheight);
    uint64_t cost_th_2 = RDCOST(full_lambda, 16, 400 * blk_geom->bwidth * blk_geom->bheight);
#endif

    if (mdctxt->md_local_blk_unit[blk_geom->sqi_mds].default_cost < cost_th_0) {
        *s_depth = 0;
        *e_depth = 0;
    }
    else if (mdctxt->md_local_blk_unit[blk_geom->sqi_mds].default_cost < cost_th_1) {
        *th_offset = -10;
    }
    else if (mdctxt->md_local_blk_unit[blk_geom->sqi_mds].default_cost < cost_th_2) {
        *th_offset = -5;
    }
}

uint8_t is_parent_to_current_deviation_small(SequenceControlSet *scs_ptr,
    ModeDecisionContext *mdctxt, const BlockGeom *blk_geom, int64_t th_offset) {

#if FTR_IMPROVE_DEPTH_REFINEMENT
    if (mdctxt->depth_refinement_ctrls.parent_to_current_th == MIN_SIGNED_VALUE)
        return EB_FALSE;
#endif
#if FTR_IMPROVE_DEPTH_REFINEMENT
    mdctxt->parent_to_current_deviation = MIN_SIGNED_VALUE;
#else
    int64_t parent_to_current_deviation = MIN_SIGNED_VALUE;
#endif
    // block-based depth refinement using cost is applicable for only [s_depth=-1, e_depth=1]
        // Get the parent of the current block
    uint32_t parent_depth_idx_mds =
        (blk_geom->sqi_mds -
        (blk_geom->quadi - 3) * ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth]) -
        parent_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
    if (mdctxt->avail_blk_flag[parent_depth_idx_mds]) {
#if FTR_IMPROVE_DEPTH_REFINEMENT
        mdctxt->parent_to_current_deviation =
#else
        parent_to_current_deviation =
#endif
            (int64_t)(((int64_t)MAX(mdctxt->md_local_blk_unit[parent_depth_idx_mds].default_cost, 1) - (int64_t)MAX((mdctxt->md_local_blk_unit[blk_geom->sqi_mds].default_cost * 4), 1)) * 100) /
            (int64_t)MAX((mdctxt->md_local_blk_unit[blk_geom->sqi_mds].default_cost * 4), 1);
    }

#if FTR_IMPROVE_DEPTH_REFINEMENT
    if (mdctxt->parent_to_current_deviation <= (mdctxt->depth_refinement_ctrls.parent_to_current_th + th_offset))
#else
    if (parent_to_current_deviation <= (mdctxt->depth_refinement_ctrls.parent_to_current_th + th_offset))
#endif
        return EB_TRUE;

    return EB_FALSE;
}

uint8_t is_child_to_current_deviation_small(SequenceControlSet *scs_ptr,
    ModeDecisionContext *mdctxt, const BlockGeom *blk_geom, uint32_t blk_index, int64_t th_offset) {

#if FTR_IMPROVE_DEPTH_REFINEMENT
    if (mdctxt->depth_refinement_ctrls.sub_to_current_th == MIN_SIGNED_VALUE)
        return EB_FALSE;
#endif
#if FTR_IMPROVE_DEPTH_REFINEMENT
    mdctxt->child_to_current_deviation = MIN_SIGNED_VALUE;
#else
    int64_t child_to_current_deviation = MIN_SIGNED_VALUE;
#endif
#if OPT6_DEPTH_REFINEMENT
    const uint32_t ns_d1_offset = d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
    const uint32_t ns_depth_plus1_offset = ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth + 1];
    const uint32_t child_block_idx_1 = blk_index + ns_d1_offset;
    const uint32_t child_block_idx_2 = child_block_idx_1 + ns_depth_plus1_offset;
    const uint32_t child_block_idx_3 = child_block_idx_2 + ns_depth_plus1_offset;
    const uint32_t child_block_idx_4 = child_block_idx_3 + ns_depth_plus1_offset;
#else
    uint32_t child_block_idx_1, child_block_idx_2, child_block_idx_3, child_block_idx_4;
    child_block_idx_1 = blk_index + d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
    child_block_idx_2 = child_block_idx_1 + ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth + 1];
    child_block_idx_3 = child_block_idx_2 + ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth + 1];
    child_block_idx_4 = child_block_idx_3 + ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth + 1];
#endif

    uint64_t child_cost = 0;
    uint8_t child_cnt = 0;
    if (mdctxt->avail_blk_flag[child_block_idx_1]) {
        child_cost += mdctxt->md_local_blk_unit[child_block_idx_1].default_cost;
        child_cnt++;
    }
    if (mdctxt->avail_blk_flag[child_block_idx_2]) {
        child_cost += mdctxt->md_local_blk_unit[child_block_idx_2].default_cost;
        child_cnt++;
    }
    if (mdctxt->avail_blk_flag[child_block_idx_3]) {
        child_cost += mdctxt->md_local_blk_unit[child_block_idx_3].default_cost;
        child_cnt++;
    }
    if (mdctxt->avail_blk_flag[child_block_idx_4]) {
        child_cost += mdctxt->md_local_blk_unit[child_block_idx_4].default_cost;
        child_cnt++;
    }
    if (child_cnt) {
        child_cost = (child_cost / child_cnt) * 4;
#if FTR_IMPROVE_DEPTH_REFINEMENT
        mdctxt->child_to_current_deviation =
#else
        child_to_current_deviation =
#endif
            (int64_t)(((int64_t)MAX(child_cost, 1) - (int64_t)MAX(mdctxt->md_local_blk_unit[blk_geom->sqi_mds].default_cost, 1)) * 100) /
            (int64_t)(MAX(mdctxt->md_local_blk_unit[blk_geom->sqi_mds].default_cost, 1));
    }


#if FTR_IMPROVE_DEPTH_REFINEMENT
    if (mdctxt->child_to_current_deviation <= (mdctxt->depth_refinement_ctrls.sub_to_current_th + th_offset))
#else
    if (child_to_current_deviation <= (mdctxt->depth_refinement_ctrls.sub_to_current_th + th_offset))
#endif
        return EB_TRUE;

    return EB_FALSE;
}
static void perform_pred_depth_refinement(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr,
                                          ModeDecisionContext *context_ptr, uint32_t sb_index) {
    MdcSbData *results_ptr = context_ptr->mdc_sb_array;
    uint32_t   blk_index   = 0;
#if OPT_BLK_REFINEMENT_PREP
    if (pcs_ptr->parent_pcs_ptr->disallow_nsq) {
#if OPT6_DEPTH_REFINEMENT
        if (context_ptr->disallow_4x4) {
            memset(results_ptr->consider_block, 0, sizeof(uint8_t)*scs_ptr->max_block_cnt);
            memset(results_ptr->split_flag, 1, sizeof(uint8_t)*scs_ptr->max_block_cnt);
            memset(results_ptr->refined_split_flag, 1, sizeof(uint8_t)*scs_ptr->max_block_cnt);
        } else {
#endif
            while (blk_index < scs_ptr->max_block_cnt) {

                const BlockGeom *blk_geom = get_blk_geom_mds(blk_index); \

                    EbBool split_flag =
                    blk_geom->sq_size > 4 ? EB_TRUE : EB_FALSE;
    #if OPT6_DEPTH_REFINEMENT
                results_ptr->consider_block[blk_index] = 0;
                results_ptr->split_flag[blk_index] = blk_geom->sq_size > 4 ? EB_TRUE : EB_FALSE;
                results_ptr->refined_split_flag[blk_index] = blk_geom->sq_size > 4 ? EB_TRUE : EB_FALSE;
    #else
                results_ptr->leaf_data_array[blk_index].consider_block = 0;
                results_ptr->leaf_data_array[blk_index].split_flag =
                    blk_geom->sq_size > 4 ? EB_TRUE : EB_FALSE;
                results_ptr->leaf_data_array[blk_index].refined_split_flag =
                    blk_geom->sq_size > 4 ? EB_TRUE : EB_FALSE;
    #endif

                blk_index +=
                    split_flag
                    ? d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth]
                    : ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
            }
#if OPT6_DEPTH_REFINEMENT
        }
#endif
    }
    else {
        // Reset mdc_sb_array data to defaults; it will be updated based on the predicted blocks (stored in md_blk_arr_nsq)
        while (blk_index < scs_ptr->max_block_cnt) {
            const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);
#if OPT6_DEPTH_REFINEMENT
            results_ptr->consider_block[blk_index] = 0;
            results_ptr->split_flag[blk_index] = blk_geom->sq_size > 4 ? EB_TRUE : EB_FALSE;
            results_ptr->refined_split_flag[blk_index] = blk_geom->sq_size > 4 ? EB_TRUE : EB_FALSE;
#else
            results_ptr->leaf_data_array[blk_index].consider_block = 0;
            results_ptr->leaf_data_array[blk_index].split_flag =
                blk_geom->sq_size > 4 ? EB_TRUE : EB_FALSE;
            results_ptr->leaf_data_array[blk_index].refined_split_flag =
                blk_geom->sq_size > 4 ? EB_TRUE : EB_FALSE;
#endif
#if !OPT_REFINEMENT_SIGNALS
            results_ptr->leaf_data_array[blk_index].pred_depth_refinement = -8;
            results_ptr->leaf_data_array[blk_index].pred_depth = -8;
#endif
            blk_index++;
        }
    }
#else
    // Reset mdc_sb_array data to defaults; it will be updated based on the predicted blocks (stored in md_blk_arr_nsq)
    while (blk_index < scs_ptr->max_block_cnt) {
        const BlockGeom *blk_geom                              = get_blk_geom_mds(blk_index);
        results_ptr->leaf_data_array[blk_index].consider_block = 0;
        results_ptr->leaf_data_array[blk_index].split_flag =
            blk_geom->sq_size > 4 ? EB_TRUE : EB_FALSE;
        results_ptr->leaf_data_array[blk_index].refined_split_flag =
            blk_geom->sq_size > 4 ? EB_TRUE : EB_FALSE;
#if !OPT_REFINEMENT_SIGNALS
        results_ptr->leaf_data_array[blk_index].pred_depth_refinement = -8;
        results_ptr->leaf_data_array[blk_index].pred_depth = -8;
#endif
        blk_index++;
    }
#endif
    results_ptr->leaf_count = 0;
    blk_index               = 0;
    while (blk_index < scs_ptr->max_block_cnt) {
        const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);
#if OPT6_DEPTH_REFINEMENT
        const unsigned   tot_d1_blocks = pcs_ptr->parent_pcs_ptr->disallow_nsq ? 1 :
            blk_geom->sq_size == 128 ? 17 :
            blk_geom->sq_size > 8 ? 25 :
            blk_geom->sq_size == 8 ? 5 :
            1;
#else
        const unsigned   tot_d1_blocks = blk_geom->sq_size == 128
            ? 17
            : blk_geom->sq_size > 8 ? 25 : blk_geom->sq_size == 8 ? 5 : 1;
#endif

        // if the parent square is inside inject this block
        uint8_t is_blk_allowed =
            pcs_ptr->slice_type != I_SLICE ? 1 : (blk_geom->sq_size < 128) ? 1 : 0;

        // derive split_flag
        EbBool split_flag = context_ptr->md_blk_arr_nsq[blk_index].split_flag;

        if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index] && is_blk_allowed) {
            if (blk_geom->shape == PART_N) {
                if (context_ptr->md_blk_arr_nsq[blk_index].split_flag == EB_FALSE) {
#if CLN_MOVE_DEPTH_REFINE_SIGS
                    int8_t s_depth = context_ptr->depth_ctrls.s_depth;
                    int8_t e_depth = context_ptr->depth_ctrls.e_depth;
#else
                    int8_t s_depth = 0;
                    int8_t e_depth = 0;

                    if (context_ptr->pd_pass == PD_PASS_0) {
                        // Shut thresholds in MR_MODE
                        if (pcs_ptr->enc_mode <= ENC_MRS) {
                            s_depth = -2;
                            e_depth = 2;
                        }
#if !TUNE_PRESETS_CLEANUP
                        else if (pcs_ptr->enc_mode <= ENC_MR) {
                            if (pcs_ptr->parent_pcs_ptr->input_resolution == INPUT_SIZE_240p_RANGE) {
                                s_depth = pcs_ptr->slice_type == I_SLICE ? -2 : -1;
                                e_depth = 2;
                            }
                            else if (pcs_ptr->parent_pcs_ptr->input_resolution <= INPUT_SIZE_720p_RANGE) {
                                s_depth = pcs_ptr->slice_type == I_SLICE ? -2 : -1;
                                e_depth = pcs_ptr->slice_type == I_SLICE ? 2 : 1;
                            }
                            else {
                                s_depth = -2;
                                e_depth = pcs_ptr->slice_type == I_SLICE ? 2 : 1;
                            }
                        }
#endif
                        else if (pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_0) {
#if TUNE_NEW_PRESETS_MR_M8
#if TUNE_PRESETS_AND_PRUNING
#if TUNE_M7_M9
#if FTR_ALIGN_SC_DETECOR
                            if (pcs_ptr->parent_pcs_ptr->sc_class1) {
#else
                            if (pcs_ptr->parent_pcs_ptr->sc_content_detected) {
#endif
                                // Always use at least [-1, +1] refinement for SC
#if TUNE_M4_REPOSITION
#if TUNE_SHIFT_PRESETS_DOWN
#if NEW_PRESETS
                                if (pcs_ptr->enc_mode <= ENC_M2) {
#else
                                if (pcs_ptr->enc_mode <= ENC_M3) {
#endif
#else
                                if (pcs_ptr->enc_mode <= ENC_M4) {
#endif
                                    s_depth = pcs_ptr->slice_type == I_SLICE ? -2 : -1;
                                    e_depth = pcs_ptr->slice_type == I_SLICE ? 2 : 1;
                                }
#else
                                if (pcs_ptr->enc_mode <= ENC_M3) {
                                    s_depth = pcs_ptr->slice_type == I_SLICE ? -2 : -1;
                                    e_depth = pcs_ptr->slice_type == I_SLICE ? 2 : 1;
                                }
#if TUNE_M4_BASE_NBASE
                                else if (pcs_ptr->enc_mode <= ENC_M4) {
                                    if (pcs_ptr->temporal_layer_index == 0) {
                                        s_depth = pcs_ptr->slice_type == I_SLICE ? -2 : -1;
                                        e_depth = pcs_ptr->slice_type == I_SLICE ? 2 : 1;
                                    }
                                    else {
                                        s_depth = -1;
                                        e_depth = 1;
                                    }
                                }
#endif
#endif
                                else {
                                    s_depth = -1;
                                    e_depth = 1;
                                }
                        }
#if TUNE_M4_REPOSITION
#if TUNE_SHIFT_PRESETS_DOWN
#if NEW_PRESETS
                            else if (pcs_ptr->enc_mode <= ENC_M2) {
#else
                            else if (pcs_ptr->enc_mode <= ENC_M3) {
#endif
#else
                            else if (pcs_ptr->enc_mode <= ENC_M4) {
#endif
#else
                            else if (pcs_ptr->enc_mode <= ENC_M3) {
#endif
#else
                            if (pcs_ptr->enc_mode <= ENC_M3) {
#endif
#else
                            if (pcs_ptr->enc_mode <= ENC_M5) {
#endif
#else
                            if (pcs_ptr->enc_mode <= ENC_M4) {
#endif
                                s_depth = pcs_ptr->slice_type == I_SLICE ? -2 : -1;
                                e_depth = pcs_ptr->slice_type == I_SLICE ?  2 :  1;
                            }
#if TUNE_M4_BASE_NBASE && !TUNE_M4_REPOSITION
                            else if (pcs_ptr->enc_mode <= ENC_M4) {
                                if (pcs_ptr->temporal_layer_index == 0) {
                                    s_depth = pcs_ptr->slice_type == I_SLICE ? -2 : -1;
                                    e_depth = pcs_ptr->slice_type == I_SLICE ? 2 : 1;
                            }
                                else {
                                    s_depth = -1;
                                    e_depth = 1;
                                }
                            }
#endif
#if OPT_M9_TXT_PRED_DEPTH_PRUNING
#if TUNE_M7_M9
#if TUNE_SHIFT_PRESETS_DOWN
                            else if (pcs_ptr->enc_mode <= ENC_M8) {
#else
                            else if (pcs_ptr->enc_mode <= ENC_M9) {
#endif
#else
                            else if (pcs_ptr->enc_mode <= ENC_M8) {
#endif
#else
                            else {
#endif
                                s_depth = -1;
                                e_depth = 1;
                            }
#if OPT_M9_TXT_PRED_DEPTH_PRUNING
                        else {
#if TUNE_M7_M9
#if FTR_M10
                            s_depth = 0;
                            e_depth = 0;
#else
                            s_depth = pcs_ptr->slice_type == I_SLICE ? -1 : 0;
                            e_depth = pcs_ptr->slice_type == I_SLICE ? 1 : 0;
#endif
#else
                            s_depth = 0;
                            e_depth = 0;
#endif
                        }
#endif
                        }
                        else {
                            s_depth = 0;
                            e_depth = 0;
                        }
                    } else if (context_ptr->pd_pass == PD_PASS_1) {
                        EbBool zero_coeff_present_flag =
                            context_ptr->md_blk_arr_nsq[blk_index].block_has_coeff == 0;
                        if (pcs_ptr->slice_type == I_SLICE) {
                            s_depth =  -1;
                            e_depth = (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_MR) ? 3 : 2;
                        }
                        else if (zero_coeff_present_flag && (pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_3 || pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_4)) {
                            s_depth = 0;
                            e_depth = 0;
                        } else

                            // This removes the SQ-versus-NSQ decision for the new MULTI_PASS_PD_LEVEL_1
                            if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_MR || pcs_ptr->parent_pcs_ptr->multi_pass_pd_level <= MULTI_PASS_PD_LEVEL_1) {
                                s_depth = -1;
                                e_depth =  1;
                            }
                            else
                            if (context_ptr->md_local_blk_unit[blk_index].best_d1_blk == blk_index) {

                            s_depth = -1;
                                e_depth = 0;
                            } else {
                                s_depth = 0;
                            e_depth = 1;
                            }
                    }

#endif
                    // Check that the start and end depth are in allowed range, given other features
                    // which restrict allowable depths
                    if (context_ptr->disallow_4x4) {
                        e_depth = (blk_geom->sq_size == 8) ? 0
                                : (blk_geom->sq_size == 16) ? MIN(1, e_depth)
                                : (blk_geom->sq_size == 32) ? MIN(2, e_depth)
                                : e_depth;
                    }
#if FTR_EARLY_DEPTH_REMOVAL
                    if (context_ptr->depth_removal_ctrls.enabled) {
                        if (context_ptr->depth_removal_ctrls.disallow_below_64x64) {
                            e_depth = (blk_geom->sq_size <= 64) ? 0
                                : (blk_geom->sq_size == 128) ? MIN(1, e_depth) : e_depth;
                        }
                        else if (context_ptr->depth_removal_ctrls.disallow_below_32x32) {
                            e_depth = (blk_geom->sq_size <= 32) ? 0
                                : (blk_geom->sq_size == 64) ? MIN(1, e_depth)
                                : (blk_geom->sq_size == 128) ? MIN(2, e_depth) : e_depth;
                        }
                        else if (context_ptr->depth_removal_ctrls.disallow_below_16x16) {
#else
                    if (context_ptr->depth_refinement_ctrls.enabled && context_ptr->depth_refinement_ctrls.disallow_below_16x16) {
#endif
                        e_depth = (blk_geom->sq_size <= 16) ? 0
                                : (blk_geom->sq_size ==  32) ? MIN(1, e_depth)
                                : (blk_geom->sq_size ==  64) ? MIN(2, e_depth)
                                : (blk_geom->sq_size == 128) ? MIN(3, e_depth) : e_depth;
                    }
#if FTR_EARLY_DEPTH_REMOVAL
                }
#endif
                    // Add current pred depth block(s)
                    for (unsigned block_1d_idx = 0; block_1d_idx < tot_d1_blocks; block_1d_idx++) {
#if OPT6_DEPTH_REFINEMENT
                        results_ptr->consider_block[blk_index + block_1d_idx] = 1;
                        results_ptr->refined_split_flag[blk_index + block_1d_idx] = EB_FALSE;
#else
                        results_ptr->leaf_data_array[blk_index + block_1d_idx].consider_block = 1;
#if !OPT_REFINEMENT_SIGNALS
                        results_ptr->leaf_data_array[blk_index + block_1d_idx].pred_depth_refinement = 0;
                        results_ptr->leaf_data_array[blk_index + block_1d_idx].pred_depth = (int8_t)blk_geom->depth;
#endif
                        results_ptr->leaf_data_array[blk_index + block_1d_idx].refined_split_flag =
                            EB_FALSE;
#endif
                    }

                    uint8_t sq_size_idx = 7 - (uint8_t)svt_log2f((uint8_t)blk_geom->sq_size);
                    // Update pred and generate an offset to be used @ sub_to_current_th and parent_to_current_th derivation based on the cost range of the predicted block; use default ths for high cost(s) and more aggressive TH(s) or Pred only for low cost(s)
                    int64_t th_offset = 0;
                    if (context_ptr->depth_refinement_ctrls.enabled && context_ptr->depth_refinement_ctrls.use_pred_block_cost && (s_depth != 0 || e_depth != 0)) {
                        update_pred_th_offset(context_ptr, blk_geom, &s_depth, &e_depth, &th_offset);
                    }
                    // Add block indices of upper depth(s)
                    // Block-based depth refinement using cost is applicable for only [s_depth=-1, e_depth=1]
                    uint8_t add_parent_depth = 1;
                    if (context_ptr->depth_refinement_ctrls.enabled && s_depth == -1 && pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_allowed[blk_index] && blk_geom->sq_size < ((scs_ptr->seq_header.sb_size == BLOCK_128X128) ? 128 : 64)) {
                        add_parent_depth = is_parent_to_current_deviation_small(
                            scs_ptr, context_ptr, blk_geom, th_offset);
                    }
#if !TUNE_IMPROVE_DEPTH_REFINEMENT
#if FTR_PD2_BLOCK_REDUCTION
                    if (context_ptr->depth_refinement_ctrls.enabled && context_ptr->depth_refinement_ctrls.use_sb_class) {
                        s_depth = context_ptr->sb_class >= SB_CLASS_1 && context_ptr->sb_class < SB_CLASS_8 ? 0 : s_depth;
                        e_depth = context_ptr->sb_class >= SB_CLASS_1 && context_ptr->sb_class < SB_CLASS_4 ? 0 : e_depth;
                    }
#endif
#endif
#if FTR_IMPROVE_DEPTH_REFINEMENT
                    // Add block indices of lower depth(s)
                    // Block-based depth refinement using cost is applicable for only [s_depth=-1, e_depth=1]
                    uint8_t add_sub_depth = 1;
                    if (context_ptr->depth_refinement_ctrls.enabled && e_depth == 1 && pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_allowed[blk_index] && blk_geom->sq_size > 4) {
                        add_sub_depth = is_child_to_current_deviation_small(
                            scs_ptr, context_ptr, blk_geom, blk_index, th_offset);
                    }
                    // Use a maximum of 2 depth per block (PRED+Parent or PRED+Sub)
                    if (context_ptr->depth_refinement_ctrls.enabled && context_ptr->depth_refinement_ctrls.up_to_2_depth) {
                        if ((s_depth == -1) && add_parent_depth && (e_depth == 1) && add_sub_depth) {
                            if (context_ptr->parent_to_current_deviation != MIN_SIGNED_VALUE && context_ptr->child_to_current_deviation != MIN_SIGNED_VALUE) {
                                if (context_ptr->parent_to_current_deviation <= context_ptr->child_to_current_deviation) {
                                    add_sub_depth = 0;
                                }
                                else {
                                    add_parent_depth = 0;
                                }
                            }
                        }
                    }
#endif
                    if (add_parent_depth)
                    if (s_depth != 0)
#if OPT6_DEPTH_REFINEMENT
                        set_parent_to_be_considered(
                            results_ptr, blk_index, scs_ptr->seq_header.sb_size, (int8_t)blk_geom->depth,sq_size_idx,
                            pcs_ptr->parent_pcs_ptr->disallow_nsq, s_depth);
#else
                        set_parent_to_be_considered(
                            results_ptr, blk_index, scs_ptr->seq_header.sb_size, (int8_t)blk_geom->depth,sq_size_idx,  s_depth);
#endif
#if !FTR_IMPROVE_DEPTH_REFINEMENT
                    // Add block indices of lower depth(s)
                    // Block-based depth refinement using cost is applicable for only [s_depth=-1, e_depth=1]
                    uint8_t add_sub_depth = 1;
                    if (context_ptr->depth_refinement_ctrls.enabled && e_depth == 1 && pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_allowed[blk_index] && blk_geom->sq_size > 4) {
                        add_sub_depth = is_child_to_current_deviation_small(
                            scs_ptr, context_ptr, blk_geom, blk_index, th_offset);
                    }
#endif
                    if (add_sub_depth)
                    if (e_depth != 0)
                        set_child_to_be_considered(pcs_ptr, context_ptr, results_ptr, blk_index, sb_index, scs_ptr->seq_header.sb_size, (int8_t)blk_geom->depth,sq_size_idx, e_depth);
                }
            }
        }
        blk_index +=
            split_flag
                ? d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth]
                : ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
    }
}
#if OPT_BUILD_CAND_BLK_3
// Initialize structures used to indicate which blocks will be tested at MD.
// MD data structures should be updated in init_block_data(), not here.
static void build_starting_cand_block_array(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr, uint32_t sb_index) {

    memset(context_ptr->tested_blk_flag, 0, sizeof(uint8_t) * scs_ptr->max_block_cnt);
    memset(context_ptr->do_not_process_blk, 0, sizeof(uint8_t) * scs_ptr->max_block_cnt);

    MdcSbData *results_ptr = context_ptr->mdc_sb_array;
    results_ptr->leaf_count = 0;
    uint32_t blk_index = 0;
#if !LIGHT_PD0
    const BlockSize sb_size = scs_ptr->seq_header.sb_size;
#endif
    const uint16_t max_block_cnt = scs_ptr->max_block_cnt;
    const int32_t min_sq_size =
        (context_ptr->depth_removal_ctrls.enabled && context_ptr->depth_removal_ctrls.disallow_below_64x64)
        ? 64
        : (context_ptr->depth_removal_ctrls.enabled && context_ptr->depth_removal_ctrls.disallow_below_32x32)
        ? 32
        : (context_ptr->depth_removal_ctrls.enabled && context_ptr->depth_removal_ctrls.disallow_below_16x16)
        ? 16
        : context_ptr->disallow_4x4 ? 8 : 4;

    // Loop over all blocks to initialize data for partitions to be tested
    while (blk_index < max_block_cnt) {
        const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);
        // SQ/NSQ block(s) filter based on the SQ size
        const uint8_t is_block_tagged =
            (blk_geom->sq_size == 128 && pcs_ptr->slice_type == I_SLICE) ||
            (blk_geom->sq_size < min_sq_size)
            ? 0
            : 1;

        // SQ/NSQ block(s) filter based on the block validity
        if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index] && is_block_tagged) {

            const uint32_t tot_d1_blocks = pcs_ptr->parent_pcs_ptr->disallow_nsq ? 1 :
                get_tot_1d_blks(pcs_ptr->parent_pcs_ptr, blk_geom->sq_size, context_ptr->md_disallow_nsq);

            for (uint32_t idx = blk_index; idx < (tot_d1_blocks + blk_index); ++idx) {

                if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_inside_md_scan[idx]) {
                    results_ptr->leaf_data_array[results_ptr->leaf_count].mds_idx = idx;
                    results_ptr->leaf_data_array[results_ptr->leaf_count].tot_d1_blocks = tot_d1_blocks;
                    results_ptr->split_flag[results_ptr->leaf_count++] = (blk_geom->sq_size > min_sq_size) ? EB_TRUE : EB_FALSE;
                }
            }
#if LIGHT_PD0
            blk_index += blk_geom->d1_depth_offset;
#else
            blk_index += d1_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth];
#endif
        }
        else {
#if LIGHT_PD0
            blk_index +=
                (blk_geom->sq_size > min_sq_size)
                ? blk_geom->d1_depth_offset
                : blk_geom->ns_depth_offset;
#else
            blk_index +=
                (blk_geom->sq_size > min_sq_size)
                ? d1_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth]
                : ns_depth_offset[sb_size == BLOCK_128X128][blk_geom->depth];
#endif
        }
    }
}
#else

void init_block(ModeDecisionContext *context_ptr, uint32_t blk_index,
                const BlockGeom *blk_geom) {
#if !OPT_BUILD_CAND_BLK_2
    context_ptr->md_local_blk_unit[blk_index].left_neighbor_partition  = +INVALID_NEIGHBOR_DATA;
    context_ptr->md_local_blk_unit[blk_index].above_neighbor_partition = +INVALID_NEIGHBOR_DATA;
#endif
#if FTR_PD2_BLOCK_REDUCTION
    context_ptr->md_local_blk_unit[blk_index].count_non_zero_coeffs = 0;
#endif
#if !CLN_NSQ_AND_STATS
    if (!context_ptr->md_disallow_nsq)
        for (uint8_t shape_idx = 0; shape_idx < NUMBER_OF_SHAPES; shape_idx++)
        context_ptr->md_local_blk_unit[blk_index].sse_gradian_band[shape_idx] = +1;
#endif
    if (blk_geom->shape == PART_N) {
        context_ptr->md_blk_arr_nsq[blk_index].split_flag         = EB_TRUE;
        context_ptr->md_blk_arr_nsq[blk_index].part               = PARTITION_SPLIT;
#if !OPT_BUILD_CAND_BLK_2
        context_ptr->md_local_blk_unit[blk_index].tested_blk_flag = EB_FALSE;
#endif
    }
#if !OPT_BUILD_CAND_BLK_2
    context_ptr->md_blk_arr_nsq[blk_index].do_not_process_block = 0;
#endif
}
static void build_starting_cand_block_array(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr, uint32_t sb_index) {

#if OPT_BUILD_CAND_BLK_2
    memset(context_ptr->tested_blk_flag, 0, sizeof(uint8_t) * scs_ptr->max_block_cnt);
    memset(context_ptr->do_not_process_blk, 0, sizeof(uint8_t) * scs_ptr->max_block_cnt);
#endif
    MdcSbData *results_ptr = context_ptr->mdc_sb_array;

    results_ptr->leaf_count = 0;
    uint32_t blk_index = 0;
    int32_t min_sq_size =
#if FTR_EARLY_DEPTH_REMOVAL
    (context_ptr->depth_removal_ctrls.enabled && context_ptr->depth_removal_ctrls.disallow_below_64x64)
        ? 64
        : (context_ptr->depth_removal_ctrls.enabled && context_ptr->depth_removal_ctrls.disallow_below_32x32)
        ? 32
        : (context_ptr->depth_removal_ctrls.enabled && context_ptr->depth_removal_ctrls.disallow_below_16x16)
#else
        (context_ptr->depth_refinement_ctrls.enabled && context_ptr->depth_refinement_ctrls.disallow_below_16x16)
#endif
            ? 16
            : context_ptr->disallow_4x4 ? 8 : 4;
    while (blk_index < scs_ptr->max_block_cnt) {
        const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);
        // SQ/NSQ block(s) filter based on the SQ size
        uint8_t is_block_tagged =
            (blk_geom->sq_size == 128 && pcs_ptr->slice_type == I_SLICE) ||
            (blk_geom->sq_size < min_sq_size)
            ? 0
            : 1;

        // SQ/NSQ block(s) filter based on the block validity
        if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index] && is_block_tagged) {
#if OPT7_BUILD_CAND_BLK
            uint32_t tot_d1_blocks = pcs_ptr->parent_pcs_ptr->disallow_nsq ? 1:
                (context_ptr->md_disallow_nsq) ||
#else
            uint32_t tot_d1_blocks = (context_ptr->md_disallow_nsq) ||
#endif
                (blk_geom->sq_size >= 64 && pcs_ptr->parent_pcs_ptr->disallow_all_nsq_blocks_above_64x64) ||
                (blk_geom->sq_size >= 32 && pcs_ptr->parent_pcs_ptr->disallow_all_nsq_blocks_above_32x32) ||
                (blk_geom->sq_size >= 16 && pcs_ptr->parent_pcs_ptr->disallow_all_nsq_blocks_above_16x16) ||
                (blk_geom->sq_size <= 64 && pcs_ptr->parent_pcs_ptr->disallow_all_nsq_blocks_below_64x64) ||
                (blk_geom->sq_size <= 32 && pcs_ptr->parent_pcs_ptr->disallow_all_nsq_blocks_below_32x32) ||
                (blk_geom->sq_size <= 8 && pcs_ptr->parent_pcs_ptr->disallow_all_nsq_blocks_below_8x8) ||
                (blk_geom->sq_size <= 16 && pcs_ptr->parent_pcs_ptr->disallow_all_nsq_blocks_below_16x16) ? 1 :
                (blk_geom->sq_size == 16 && pcs_ptr->parent_pcs_ptr->disallow_all_non_hv_nsq_blocks_below_16x16) ? 5 :
                (blk_geom->sq_size == 16 && pcs_ptr->parent_pcs_ptr->disallow_all_h4_v4_blocks_below_16x16) ? 17 :
                blk_geom->sq_size == 128
                ? 17
                : blk_geom->sq_size > 8 ? 25 : blk_geom->sq_size == 8 ? 5 : 1;

            if (pcs_ptr->parent_pcs_ptr->disallow_HVA_HVB_HV4)
                tot_d1_blocks = MIN(5, tot_d1_blocks);

            if (pcs_ptr->parent_pcs_ptr->disallow_HV4)
                tot_d1_blocks = MIN(17, tot_d1_blocks);
            for (uint32_t idx = 0; idx < tot_d1_blocks; ++idx) {
                blk_geom = get_blk_geom_mds(blk_index);

                if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index]) {
                    init_block(context_ptr, blk_index, blk_geom);
                    results_ptr->leaf_data_array[results_ptr->leaf_count].mds_idx = blk_index;
                    results_ptr->leaf_data_array[results_ptr->leaf_count].tot_d1_blocks = tot_d1_blocks;
#if !OPT_REFINEMENT_SIGNALS
                    results_ptr->leaf_data_array[results_ptr->leaf_count].final_pred_depth_refinement = 0;
#endif
#if OPT6_DEPTH_REFINEMENT
                    if (blk_geom->sq_size > min_sq_size)
                        results_ptr->split_flag[results_ptr->leaf_count++] = EB_TRUE;
                    else
                        results_ptr->split_flag[results_ptr->leaf_count++] = EB_FALSE;
#else
                    if (blk_geom->sq_size > min_sq_size)
                        results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag =
                        EB_TRUE;
                    else
                        results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag =
                        EB_FALSE;
#endif
                }
                blk_index++;
            }
            blk_index +=
                (d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth] -
                    tot_d1_blocks);
        }
        else {
            blk_index +=
                (blk_geom->sq_size > min_sq_size)
                ? d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth]
                : ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
        }
    }
}

#endif
void recode_loop_update_q(
    PictureParentControlSet *ppcs_ptr,
    int *const loop, int *const q, int *const q_low,
    int *const q_high, const int top_index, const int bottom_index,
    int *const undershoot_seen, int *const overshoot_seen,
    int *const low_cr_seen, const int loop_count);
void sb_qp_derivation_tpl_la(PictureControlSet *pcs_ptr);
void mode_decision_configuration_init_qp_update(PictureControlSet *pcs_ptr);
void init_enc_dec_segement(PictureParentControlSet *parentpicture_control_set_ptr);

static void recode_loop_decision_maker(PictureControlSet *pcs_ptr,
            SequenceControlSet *scs_ptr, EbBool *do_recode) {
    PictureParentControlSet *ppcs_ptr = pcs_ptr->parent_pcs_ptr;
    EncodeContext *const encode_context_ptr = ppcs_ptr->scs_ptr->encode_context_ptr;
    RATE_CONTROL *const rc = &(encode_context_ptr->rc);
    int32_t loop = 0;
    FrameHeader *frm_hdr = &ppcs_ptr->frm_hdr;
    int32_t q = frm_hdr->quantization_params.base_q_idx;
    if (ppcs_ptr->loop_count == 0) {
#if FTR_VBR_MT
        ppcs_ptr->q_low = ppcs_ptr->bottom_index;
        ppcs_ptr->q_high = ppcs_ptr->top_index;
#else
        ppcs_ptr->q_low  = rc->bottom_index;
        ppcs_ptr->q_high = rc->top_index;
#endif
    }

    // Update q and decide whether to do a recode loop
    recode_loop_update_q(ppcs_ptr, &loop, &q,
            &ppcs_ptr->q_low, &ppcs_ptr->q_high,
#if FTR_VBR_MT
            ppcs_ptr->top_index, ppcs_ptr->bottom_index,
#else
            rc->top_index, rc->bottom_index,
#endif
            &ppcs_ptr->undershoot_seen, &ppcs_ptr->overshoot_seen,
            &ppcs_ptr->low_cr_seen, ppcs_ptr->loop_count);

    // Special case for overlay frame.
#if FTR_VBR_MT
    if (loop && ppcs_ptr->is_src_frame_alt_ref &&
        ppcs_ptr->projected_frame_size < rc->max_frame_bandwidth) {
#else
    if (loop && rc->is_src_frame_alt_ref &&
        rc->projected_frame_size < rc->max_frame_bandwidth) {
#endif
        loop = 0;
    }
    *do_recode = loop == 1;

    if (*do_recode) {
        ppcs_ptr->loop_count++;

        frm_hdr->quantization_params.base_q_idx = (uint8_t)CLIP3(
                (int32_t)quantizer_to_qindex[scs_ptr->static_config.min_qp_allowed],
                (int32_t)quantizer_to_qindex[scs_ptr->static_config.max_qp_allowed],
                q);

        ppcs_ptr->picture_qp =
            (uint8_t)CLIP3((int32_t)scs_ptr->static_config.min_qp_allowed,
                    (int32_t)scs_ptr->static_config.max_qp_allowed,
                    (frm_hdr->quantization_params.base_q_idx + 2) >> 2);
        pcs_ptr->picture_qp = ppcs_ptr->picture_qp;

        // 2pass QPM with tpl_la
        if (scs_ptr->static_config.enable_adaptive_quantization == 2 &&
            !use_output_stat(scs_ptr) &&
            (use_input_stat(scs_ptr) || scs_ptr->lap_enabled) &&
            scs_ptr->static_config.enable_tpl_la &&
            ppcs_ptr->r0 != 0)
            sb_qp_derivation_tpl_la(pcs_ptr);
        else
        {
            ppcs_ptr->frm_hdr.delta_q_params.delta_q_present = 0;
            ppcs_ptr->average_qp = 0;
            for (int sb_addr = 0; sb_addr < pcs_ptr->sb_total_count_pix; ++sb_addr) {
                SuperBlock * sb_ptr = pcs_ptr->sb_ptr_array[sb_addr];
                sb_ptr->qindex   = quantizer_to_qindex[pcs_ptr->picture_qp];
                ppcs_ptr->average_qp += pcs_ptr->picture_qp;
            }
        }
    } else {
        ppcs_ptr->loop_count = 0;
    }
}
static void init_avail_blk_flag(SequenceControlSet *scs_ptr, ModeDecisionContext *context_ptr) {
    // Initialize avail_blk_flag to false
    memset(context_ptr->avail_blk_flag, EB_FALSE, sizeof(uint8_t) * scs_ptr->max_block_cnt);
}
/* EncDec (Encode Decode) Kernel */
/*********************************************************************************
*
* @brief
*  The EncDec process contains both the mode decision and the encode pass engines
*  of the encoder. The mode decision encapsulates multiple partitioning decision (PD) stages
*  and multiple mode decision (MD) stages. At the end of the last mode decision stage,
*  the winning partition and modes combinations per block get reconstructed in the encode pass
*  operation which is part of the common section between the encoder and the decoder
*  Common encoder and decoder tasks such as Intra Prediction, Motion Compensated Prediction,
*  Transform, Quantization are performed in this process.
*
* @par Description:
*  The EncDec process operates on an SB basis.
*  The EncDec process takes as input the Motion Vector XY pairs candidates
*  and corresponding distortion estimates from the Motion Estimation process,
*  and the picture-level QP from the Rate Control process. All inputs are passed
*  through the picture structures: PictureControlSet and SequenceControlSet.
*  local structures of type EncDecContext and ModeDecisionContext contain all parameters
*  and results corresponding to the SuperBlock being processed.
*  each of the context structures is local to on thread and thus there's no risk of
*  affecting (changing) other SBs data in the process.
*
* @param[in] Vector
*  Motion Vector XY pairs from Motion Estimation process
*
* @param[in] Distortion Estimates
*  Distortion estimates from Motion Estimation process
*
* @param[in] Picture QP
*  Picture Quantization Parameter from Rate Control process
*
* @param[out] Blocks
*  The encode pass takes the selected partitioning and coding modes as input from mode decision for each
*  superblock and produces quantized transfrom coefficients for the residuals and the appropriate syntax
*  elements to be sent to the entropy coding engine
*
********************************************************************************/
void *mode_decision_kernel(void *input_ptr) {
    // Context & SCS & PCS
    EbThreadContext *   thread_context_ptr = (EbThreadContext *)input_ptr;
    EncDecContext *     context_ptr        = (EncDecContext *)thread_context_ptr->priv;

    // Input
    EbObjectWrapper *enc_dec_tasks_wrapper_ptr;

    // Output
    EbObjectWrapper *enc_dec_results_wrapper_ptr;
    EncDecResults *  enc_dec_results_ptr;
    // SB Loop variables
    SuperBlock *sb_ptr;
    uint16_t    sb_index;
    uint32_t    x_sb_index;
    uint32_t    y_sb_index;
    uint32_t    sb_origin_x;
    uint32_t    sb_origin_y;
    MdcSbData * mdc_ptr;

    // Segments
    uint16_t        segment_index;
    uint32_t        x_sb_start_index;
    uint32_t        y_sb_start_index;
    uint32_t        sb_start_index;
    uint32_t        sb_segment_count;
    uint32_t        sb_segment_index;
    uint32_t        segment_row_index;
    uint32_t        segment_band_index;
    uint32_t        segment_band_size;
    EncDecSegments *segments_ptr;

    segment_index = 0;

    for (;;) {
        // Get Mode Decision Results
        EB_GET_FULL_OBJECT(context_ptr->mode_decision_input_fifo_ptr, &enc_dec_tasks_wrapper_ptr);

        EncDecTasks *    enc_dec_tasks_ptr    = (EncDecTasks *)enc_dec_tasks_wrapper_ptr->object_ptr;
        PictureControlSet * pcs_ptr           = (PictureControlSet *)enc_dec_tasks_ptr->pcs_wrapper_ptr->object_ptr;
        SequenceControlSet *scs_ptr           = (SequenceControlSet *)pcs_ptr->scs_wrapper_ptr->object_ptr;

        context_ptr->tile_group_index = enc_dec_tasks_ptr->tile_group_index;
        context_ptr->coded_sb_count   = 0;
        segments_ptr = pcs_ptr->enc_dec_segment_ctrl[context_ptr->tile_group_index];
        // SB Constants
        uint8_t sb_sz      = (uint8_t)scs_ptr->sb_size_pix;
        uint8_t sb_size_log2 = (uint8_t)svt_log2f(sb_sz);
        context_ptr->sb_sz = sb_sz;
        uint32_t pic_width_in_sb = (pcs_ptr->parent_pcs_ptr->aligned_width + sb_sz - 1) >>
            sb_size_log2;
        uint16_t tile_group_width_in_sb = pcs_ptr->parent_pcs_ptr
                                              ->tile_group_info[context_ptr->tile_group_index]
                                              .tile_group_width_in_sb;
#if !TUNE_REMOVE_INTRA_STATS_TRACKING
        context_ptr->tot_intra_coded_area       = 0;
#endif
        // Bypass encdec for the first pass
        if (use_output_stat(scs_ptr)) {

            svt_release_object(pcs_ptr->parent_pcs_ptr->me_data_wrapper_ptr);
            pcs_ptr->parent_pcs_ptr->me_data_wrapper_ptr = (EbObjectWrapper *)NULL;
            // Get Empty EncDec Results
            svt_get_empty_object(context_ptr->enc_dec_output_fifo_ptr, &enc_dec_results_wrapper_ptr);
            enc_dec_results_ptr = (EncDecResults *)enc_dec_results_wrapper_ptr->object_ptr;
            enc_dec_results_ptr->pcs_wrapper_ptr = enc_dec_tasks_ptr->pcs_wrapper_ptr;
            enc_dec_results_ptr->completed_sb_row_index_start = 0;
            enc_dec_results_ptr->completed_sb_row_count =
                ((pcs_ptr->parent_pcs_ptr->aligned_height + scs_ptr->sb_size_pix - 1) >> sb_size_log2);
            // Post EncDec Results
            svt_post_full_object(enc_dec_results_wrapper_ptr);
        }
        else{
#if !CLN_REMOVE_UNUSED_CODE
        memset(context_ptr->md_context->part_cnt, 0, sizeof(uint32_t) * SSEG_NUM * (NUMBER_OF_SHAPES-1) * FB_NUM);
        generate_nsq_prob(pcs_ptr, context_ptr->md_context);
#endif
#if !CLN_NSQ_AND_STATS
        memset(context_ptr->md_context->pred_depth_count, 0, sizeof(uint32_t) * DEPTH_DELTA_NUM * (NUMBER_OF_SHAPES-1));
        generate_depth_prob(pcs_ptr, context_ptr->md_context);
#endif
#if !TUNE_REMOVE_TXT_STATS
        memset( context_ptr->md_context->txt_cnt, 0, sizeof(uint32_t) * TXT_DEPTH_DELTA_NUM * TX_TYPES);
        generate_txt_prob(pcs_ptr, context_ptr->md_context);
#endif
        if (!pcs_ptr->cdf_ctrl.update_mv)
            copy_mv_rate(pcs_ptr, &context_ptr->md_context->rate_est_table);
        if (!pcs_ptr->cdf_ctrl.update_se)
            av1_estimate_syntax_rate(&context_ptr->md_context->rate_est_table,
                pcs_ptr->slice_type == I_SLICE ? EB_TRUE : EB_FALSE,
                &pcs_ptr->md_frame_context);
        if (!pcs_ptr->cdf_ctrl.update_coef)
            av1_estimate_coefficients_rate(&context_ptr->md_context->rate_est_table,
                &pcs_ptr->md_frame_context);
        // Segment-loop
        while (assign_enc_dec_segments(segments_ptr,
                                       &segment_index,
                                       enc_dec_tasks_ptr,
                                       context_ptr->enc_dec_feedback_fifo_ptr) == EB_TRUE) {
            x_sb_start_index = segments_ptr->x_start_array[segment_index];
            y_sb_start_index = segments_ptr->y_start_array[segment_index];
            sb_start_index = y_sb_start_index * tile_group_width_in_sb + x_sb_start_index;
            sb_segment_count = segments_ptr->valid_sb_count_array[segment_index];

            segment_row_index = segment_index / segments_ptr->segment_band_count;
            segment_band_index =
                segment_index - segment_row_index * segments_ptr->segment_band_count;
            segment_band_size = (segments_ptr->sb_band_count * (segment_band_index + 1) +
                                 segments_ptr->segment_band_count - 1) /
                                segments_ptr->segment_band_count;

            // Reset Coding Loop State
            reset_mode_decision(scs_ptr,
                                context_ptr->md_context,
                                pcs_ptr,
                                context_ptr->tile_group_index,
                                segment_index);

            // Reset EncDec Coding State
            reset_enc_dec( // HT done
                context_ptr,
                pcs_ptr,
                scs_ptr,
                segment_index);

            if (pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr != NULL)
                ((EbReferenceObject *)
                     pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
                    ->average_intensity = pcs_ptr->parent_pcs_ptr->average_intensity[0];
            for (y_sb_index = y_sb_start_index, sb_segment_index = sb_start_index;
                 sb_segment_index < sb_start_index + sb_segment_count;
                 ++y_sb_index) {
                for (x_sb_index = x_sb_start_index;
                     x_sb_index < tile_group_width_in_sb &&
                     (x_sb_index + y_sb_index < segment_band_size) &&
                     sb_segment_index < sb_start_index + sb_segment_count;
                     ++x_sb_index, ++sb_segment_index) {
                    uint16_t tile_group_y_sb_start =
                        pcs_ptr->parent_pcs_ptr->tile_group_info[context_ptr->tile_group_index]
                            .tile_group_sb_start_y;
                    uint16_t tile_group_x_sb_start =
                        pcs_ptr->parent_pcs_ptr->tile_group_info[context_ptr->tile_group_index]
                            .tile_group_sb_start_x;
                    sb_index = context_ptr->md_context->sb_index =(uint16_t)((y_sb_index + tile_group_y_sb_start) * pic_width_in_sb +
                        x_sb_index + tile_group_x_sb_start);
                    sb_ptr = context_ptr->md_context->sb_ptr = pcs_ptr->sb_ptr_array[sb_index];
                    sb_origin_x = (x_sb_index + tile_group_x_sb_start) << sb_size_log2;
                    sb_origin_y = (y_sb_index + tile_group_y_sb_start) << sb_size_log2;
                    //printf("[%ld]:ED sb index %d, (%d, %d), encoded total sb count %d, ctx coded sb count %d\n",
                    //        pcs_ptr->picture_number,
                    //        sb_index, sb_origin_x, sb_origin_y,
                    //        pcs_ptr->enc_dec_coded_sb_count,
                    //        context_ptr->coded_sb_count);
                    context_ptr->tile_index             = sb_ptr->tile_info.tile_rs_index;
                    context_ptr->md_context->tile_index = sb_ptr->tile_info.tile_rs_index;
                    context_ptr->md_context->sb_origin_x = sb_origin_x;
                    context_ptr->md_context->sb_origin_y = sb_origin_y;
                    mdc_ptr = context_ptr->md_context->mdc_sb_array;
                    context_ptr->sb_index = sb_index;
#if !OPT_SB_CLASS
                    context_ptr->md_context->sb_class = NONE_CLASS;
#endif
                    if (pcs_ptr->cdf_ctrl.enabled) {
                        if (scs_ptr->seq_header.pic_based_rate_est &&
                            scs_ptr->enc_dec_segment_row_count_array[pcs_ptr->temporal_layer_index] == 1 &&
                            scs_ptr->enc_dec_segment_col_count_array[pcs_ptr->temporal_layer_index] == 1) {
                            if (sb_index == 0)
                                pcs_ptr->ec_ctx_array[sb_index] =  pcs_ptr->md_frame_context;
                            else
                                pcs_ptr->ec_ctx_array[sb_index] = pcs_ptr->ec_ctx_array[sb_index - 1];
                        }
                        else {
                            // Use the latest available CDF for the current SB
                            // Use the weighted average of left (3x) and top right (1x) if available.
                            int8_t top_right_available =
                                ((int32_t)(sb_origin_y >> MI_SIZE_LOG2) >
                                 sb_ptr->tile_info.mi_row_start) &&
                                ((int32_t)((sb_origin_x + (1 << sb_size_log2)) >> MI_SIZE_LOG2) <
                                 sb_ptr->tile_info.mi_col_end);

                            int8_t left_available = ((int32_t)(sb_origin_x >> MI_SIZE_LOG2) >
                                                     sb_ptr->tile_info.mi_col_start);

                            if (!left_available && !top_right_available)
                                pcs_ptr->ec_ctx_array[sb_index] =
                                  pcs_ptr->md_frame_context;
                            else if (!left_available)
                                pcs_ptr->ec_ctx_array[sb_index] =
                                    pcs_ptr->ec_ctx_array[sb_index - pic_width_in_sb + 1];
                            else if (!top_right_available)
                                pcs_ptr->ec_ctx_array[sb_index] =
                                    pcs_ptr->ec_ctx_array[sb_index - 1];
                            else {
                                pcs_ptr->ec_ctx_array[sb_index] =
                                    pcs_ptr->ec_ctx_array[sb_index - 1];
                                avg_cdf_symbols(
                                    &pcs_ptr->ec_ctx_array[sb_index],
                                    &pcs_ptr->ec_ctx_array[sb_index - pic_width_in_sb + 1],
                                    AVG_CDF_WEIGHT_LEFT,
                                    AVG_CDF_WEIGHT_TOP);
                            }
                        }
                        // Initial Rate Estimation of the syntax elements
                        if (pcs_ptr->cdf_ctrl.update_se)
                        av1_estimate_syntax_rate(&context_ptr->md_context->rate_est_table,
                            pcs_ptr->slice_type == I_SLICE,
                            &pcs_ptr->ec_ctx_array[sb_index]);
                        // Initial Rate Estimation of the Motion vectors
                        if (pcs_ptr->cdf_ctrl.update_mv)
                        av1_estimate_mv_rate(pcs_ptr,
                            &context_ptr->md_context->rate_est_table,
                            &pcs_ptr->ec_ctx_array[sb_index]);

                        if (pcs_ptr->cdf_ctrl.update_coef)
                        av1_estimate_coefficients_rate(&context_ptr->md_context->rate_est_table,
                            &pcs_ptr->ec_ctx_array[sb_index]);
#if !CLN_FAST_COST
                        //let the candidate point to the new rate table.
                        uint32_t cand_index;
                        for (cand_index = 0; cand_index < MODE_DECISION_CANDIDATE_MAX_COUNT;
                            ++cand_index)
                            context_ptr->md_context->fast_candidate_ptr_array[cand_index]
                            ->md_rate_estimation_ptr = &context_ptr->md_context->rate_est_table;
#endif
                        context_ptr->md_context->md_rate_estimation_ptr =
                            &context_ptr->md_context->rate_est_table;
                    }
                    // Configure the SB
                    mode_decision_configure_sb(
                        context_ptr->md_context, pcs_ptr, (uint8_t)sb_ptr->qindex);
#if FTR_EARLY_DEPTH_REMOVAL
                    // signals set once per SB (i.e. not per PD)
                    signal_derivation_enc_dec_kernel_common(scs_ptr, pcs_ptr, context_ptr->md_context);

                    uint8_t pd_pass_2_only = (scs_ptr->static_config.super_block_size == 64 && context_ptr->md_context->depth_removal_ctrls.disallow_below_64x64)
                        ? 1
                        : 0;

                    // Multi-Pass PD
                    if (!pd_pass_2_only &&
                        (pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_0 ||
                            pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_1 ||
                            pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_2 ||
                            pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_3 ||
                            pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_4)
                        ) {
#else
                    // Multi-Pass PD
                    if ((pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_0 ||
                         pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_1 ||
                         pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_2 ||
                         pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_3 ||
                         pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_4)
                        ) {
#endif
                        // Save a clean copy of the neighbor arrays
                        copy_neighbour_arrays(pcs_ptr,
                                              context_ptr->md_context,
                                              MD_NEIGHBOR_ARRAY_INDEX,
                                              MULTI_STAGE_PD_NEIGHBOR_ARRAY_INDEX,
                                              0,
                                              sb_origin_x,
                                              sb_origin_y);

                        // [PD_PASS_0] Signal(s) derivation
                        context_ptr->md_context->pd_pass = PD_PASS_0;
                        signal_derivation_enc_dec_kernel_oq(scs_ptr, pcs_ptr, context_ptr->md_context);

                        // [PD_PASS_0]
                        // Input : mdc_blk_ptr built @ mdc process (up to 4421)
                        // Output: md_blk_arr_nsq reduced set of block(s)

                        // Build the t=0 cand_block_array
                        build_starting_cand_block_array(scs_ptr, pcs_ptr, context_ptr->md_context, sb_index);
                        // Initialize avail_blk_flag to false
                        init_avail_blk_flag(scs_ptr, context_ptr->md_context);

                        // PD0 MD Tool(s) : ME_MV(s) as INTER candidate(s), DC as INTRA candidate, luma only, Frequency domain SSE,
                        // no fast rate (no MVP table generation), MDS0 then MDS3, reduced NIC(s), 1 ref per list,..
                        mode_decision_sb(scs_ptr,
                                         pcs_ptr,
                                         mdc_ptr,
                                         sb_ptr,
                                         sb_origin_x,
                                         sb_origin_y,
                                         sb_index,
                                         context_ptr->md_context);
#if !OPT_SB_CLASS
                            context_ptr->md_context->sb_class = determine_sb_class(
                                scs_ptr, pcs_ptr, context_ptr->md_context, sb_index);
#endif
                        // Perform Pred_0 depth refinement - add depth(s) to be considered in the next stage(s)
                        perform_pred_depth_refinement(
                            scs_ptr, pcs_ptr, context_ptr->md_context, sb_index);

                        // Re-build mdc_blk_ptr for the 2nd PD Pass [PD_PASS_1]
                        // Reset neighnor information to current SB @ position (0,0)
                        copy_neighbour_arrays(pcs_ptr,
                                              context_ptr->md_context,
                                              MULTI_STAGE_PD_NEIGHBOR_ARRAY_INDEX,
                                              MD_NEIGHBOR_ARRAY_INDEX,
                                              0,
                                              sb_origin_x,
                                              sb_origin_y);

                        if (pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_1 ||
                            pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_2 ||
                            pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_3 ||
                            pcs_ptr->parent_pcs_ptr->multi_pass_pd_level == MULTI_PASS_PD_LEVEL_4) {
                            // [PD_PASS_1] Signal(s) derivation
                            context_ptr->md_context->pd_pass = PD_PASS_1;
                            signal_derivation_enc_dec_kernel_oq(scs_ptr, pcs_ptr, context_ptr->md_context);
                            // Re-build mdc_blk_ptr for the 2nd PD Pass [PD_PASS_1]

#if OPT_BUILD_CAND_BLK_3
                            build_cand_block_array(scs_ptr, pcs_ptr, context_ptr->md_context, sb_index, pcs_ptr->parent_pcs_ptr->sb_params_array[sb_index].is_complete_sb);
#else
                            build_cand_block_array(scs_ptr, pcs_ptr, context_ptr->md_context, sb_index);
#endif
                            // Initialize avail_blk_flag to false
                            init_avail_blk_flag(scs_ptr, context_ptr->md_context);

                            // [PD_PASS_1] Mode Decision - Further reduce the number of
                            // depth(s) to be considered in later PD stages. This pass uses more accurate
                            // info than PD0 to give a better PD estimate.
                            // Input : mdc_blk_ptr built @ PD0 refinement
                            // Output: md_blk_arr_nsq reduced set of block(s)

                            // PD1 MD Tool(s): PME,..
                            mode_decision_sb(scs_ptr,
                                             pcs_ptr,
                                             mdc_ptr,
                                             sb_ptr,
                                             sb_origin_x,
                                             sb_origin_y,
                                             sb_index,
                                             context_ptr->md_context);

                            // Perform Pred_1 depth refinement - add depth(s) to be considered in the next stage(s)
                            perform_pred_depth_refinement(
                                scs_ptr, pcs_ptr, context_ptr->md_context, sb_index);
                            // Reset neighnor information to current SB @ position (0,0)
                            copy_neighbour_arrays(pcs_ptr,
                                                  context_ptr->md_context,
                                                  MULTI_STAGE_PD_NEIGHBOR_ARRAY_INDEX,
                                                  MD_NEIGHBOR_ARRAY_INDEX,
                                                  0,
                                                  sb_origin_x,
                                                  sb_origin_y);
                        }
                    }
                    // [PD_PASS_2] Signal(s) derivation
                    context_ptr->md_context->pd_pass = PD_PASS_2;
                        signal_derivation_enc_dec_kernel_oq(scs_ptr, pcs_ptr, context_ptr->md_context);
                    // Re-build mdc_blk_ptr for the 3rd PD Pass [PD_PASS_2]
#if FTR_EARLY_DEPTH_REMOVAL
                    if (!pd_pass_2_only && pcs_ptr->parent_pcs_ptr->multi_pass_pd_level != MULTI_PASS_PD_OFF)
#else
                    if(pcs_ptr->parent_pcs_ptr->multi_pass_pd_level != MULTI_PASS_PD_OFF)
#endif
#if OPT_BUILD_CAND_BLK_3
                        build_cand_block_array(scs_ptr, pcs_ptr, context_ptr->md_context, sb_index, pcs_ptr->parent_pcs_ptr->sb_params_array[sb_index].is_complete_sb);
#else
                    build_cand_block_array(scs_ptr, pcs_ptr, context_ptr->md_context, sb_index);
#endif
                    else
                        // Build the t=0 cand_block_array
                        build_starting_cand_block_array(scs_ptr, pcs_ptr, context_ptr->md_context, sb_index);
                    // Initialize avail_blk_flag to false
                    init_avail_blk_flag(scs_ptr, context_ptr->md_context);

                    // [PD_PASS_2] Mode Decision - Obtain the final partitioning decision using more accurate info
                    // than previous stages.  Reduce the total number of partitions to 1.
                    // Input : mdc_blk_ptr built @ PD1 refinement
                    // Output: md_blk_arr_nsq reduced set of block(s)

                    // PD2 MD Tool(s): default MD Tool(s)
                    mode_decision_sb(scs_ptr,
                                     pcs_ptr,
                                     mdc_ptr,
                                     sb_ptr,
                                     sb_origin_x,
                                     sb_origin_y,
                                     sb_index,
                                     context_ptr->md_context);
#if !CLN_REMOVE_UNUSED_CODE
                    generate_statistics_nsq(scs_ptr, pcs_ptr, context_ptr->md_context, sb_index);
#endif
#if !CLN_NSQ_AND_STATS
                    generate_statistics_depth(scs_ptr, pcs_ptr, context_ptr->md_context, sb_index);
#endif
#if !TUNE_REMOVE_TXT_STATS
                    generate_statistics_txt(scs_ptr, pcs_ptr, context_ptr->md_context, sb_index);
#endif
#if NO_ENCDEC
                    no_enc_dec_pass(scs_ptr,
                                    pcs_ptr,
                                    sb_ptr,
                                    sb_index,
                                    sb_origin_x,
                                    sb_origin_y,
                                    sb_ptr->qp,
                                    context_ptr);
#else
                    // Encode Pass
                    av1_encode_decode(
                        scs_ptr, pcs_ptr, sb_ptr, sb_index, sb_origin_x, sb_origin_y, context_ptr);
#endif

                    context_ptr->coded_sb_count++;
#if !TUNE_REMOVE_INTRA_STATS_TRACKING
                    if (pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr != NULL)
                        ((EbReferenceObject *)
                             pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
                            ->intra_coded_area_sb[sb_index] = (uint8_t)(
                            (100 * context_ptr->intra_coded_area_sb[sb_index]) / (64 * 64));
#endif
                }
                x_sb_start_index = (x_sb_start_index > 0) ? x_sb_start_index - 1 : 0;
            }
        }

        svt_block_on_mutex(pcs_ptr->intra_mutex);
#if !TUNE_REMOVE_INTRA_STATS_TRACKING
        pcs_ptr->intra_coded_area += (uint32_t)context_ptr->tot_intra_coded_area;
#endif
        // Accumulate block selection
#if !CLN_REMOVE_UNUSED_CODE
        for (uint8_t partidx = 0; partidx < NUMBER_OF_SHAPES-1; partidx++)
            for (uint8_t band = 0; band < FB_NUM; band++)
                for (uint8_t sse_idx = 0; sse_idx < SSEG_NUM; sse_idx++)
                    pcs_ptr->part_cnt[partidx][band][sse_idx] += context_ptr->md_context->part_cnt[partidx][band][sse_idx];
#endif
#if !CLN_NSQ_AND_STATS
        // Accumulate pred depth selection
        for (uint8_t pred_depth = 0; pred_depth < DEPTH_DELTA_NUM; pred_depth++)
            for (uint8_t part_idx = 0; part_idx < (NUMBER_OF_SHAPES-1); part_idx++)
                pcs_ptr->pred_depth_count[pred_depth][part_idx] += context_ptr->md_context->pred_depth_count[pred_depth][part_idx];
#endif
#if !TUNE_REMOVE_TXT_STATS
        // Accumulate tx_type selection
        for (uint8_t depth_delta = 0; depth_delta < TXT_DEPTH_DELTA_NUM; depth_delta++)
            for (uint8_t txs_idx = 0; txs_idx < TX_TYPES; txs_idx++)
                pcs_ptr->txt_cnt[depth_delta][txs_idx] += context_ptr->md_context->txt_cnt[depth_delta][txs_idx];
#endif
        pcs_ptr->enc_dec_coded_sb_count += (uint32_t)context_ptr->coded_sb_count;
        EbBool last_sb_flag = (pcs_ptr->sb_total_count_pix == pcs_ptr->enc_dec_coded_sb_count);
        svt_release_mutex(pcs_ptr->intra_mutex);

        if (last_sb_flag) {
            EbBool do_recode = EB_FALSE;
            scs_ptr->encode_context_ptr->recode_loop = scs_ptr->static_config.recode_loop;
            if ((use_input_stat(scs_ptr) || scs_ptr->lap_enabled) &&
                scs_ptr->encode_context_ptr->recode_loop != DISALLOW_RECODE) {
                recode_loop_decision_maker(pcs_ptr, scs_ptr, &do_recode);
            }

            if (do_recode) {

                pcs_ptr->enc_dec_coded_sb_count = 0;
                // Reset MD rate Estimation table to initial values by copying from md_rate_estimation_array
                if (context_ptr->is_md_rate_estimation_ptr_owner) {
                    EB_FREE_ARRAY(context_ptr->md_rate_estimation_ptr);
                    context_ptr->is_md_rate_estimation_ptr_owner = EB_FALSE;
                }
                context_ptr->md_rate_estimation_ptr = pcs_ptr->md_rate_estimation_array;
                // re-init mode decision configuration for qp update for re-encode frame
                mode_decision_configuration_init_qp_update(pcs_ptr);
                // init segment for re-encode frame
                init_enc_dec_segement(pcs_ptr->parent_pcs_ptr);
                EbObjectWrapper *enc_dec_re_encode_tasks_wrapper_ptr;
                uint16_t tg_count =
                    pcs_ptr->parent_pcs_ptr->tile_group_cols * pcs_ptr->parent_pcs_ptr->tile_group_rows;
                for (uint16_t tile_group_idx = 0; tile_group_idx < tg_count; tile_group_idx++) {
                    svt_get_empty_object(context_ptr->enc_dec_feedback_fifo_ptr,
                            &enc_dec_re_encode_tasks_wrapper_ptr);

                    EncDecTasks *enc_dec_re_encode_tasks_ptr = (EncDecTasks *)enc_dec_re_encode_tasks_wrapper_ptr->object_ptr;
                    enc_dec_re_encode_tasks_ptr->pcs_wrapper_ptr  = enc_dec_tasks_ptr->pcs_wrapper_ptr;
                    enc_dec_re_encode_tasks_ptr->input_type       = ENCDEC_TASKS_MDC_INPUT;
                    enc_dec_re_encode_tasks_ptr->tile_group_index = tile_group_idx;

                    // Post the Full Results Object
                    svt_post_full_object(enc_dec_re_encode_tasks_wrapper_ptr);
                }

            }
            else {
            // Copy film grain data from parent picture set to the reference object for further reference
            if (scs_ptr->seq_header.film_grain_params_present) {
                if (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag == EB_TRUE &&
                    pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr) {
                    ((EbReferenceObject *)
                         pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
                        ->film_grain_params = pcs_ptr->parent_pcs_ptr->frm_hdr.film_grain_params;
                }
            }
#if FIX_FE_CDF_UPDATE_CRASH_NBASE
            // Force each frame to update their data so future frames can use it,
            // even if the current frame did not use it.  This enables REF frames to
            // have the feature off, while NREF frames can have it on.  Used for multi-threading.
            if (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag == EB_TRUE &&
#else
            if (pcs_ptr->parent_pcs_ptr->frame_end_cdf_update_mode &&
                pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag == EB_TRUE &&
#endif
                pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr)
                for (int frame = LAST_FRAME; frame <= ALTREF_FRAME; ++frame)
                    ((EbReferenceObject *)
                         pcs_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)
                        ->global_motion[frame] = pcs_ptr->parent_pcs_ptr->global_motion[frame];
            svt_memcpy(pcs_ptr->parent_pcs_ptr->av1x->sgrproj_restore_cost,
                       context_ptr->md_rate_estimation_ptr->sgrproj_restore_fac_bits,
                       2 * sizeof(int32_t));
            svt_memcpy(pcs_ptr->parent_pcs_ptr->av1x->switchable_restore_cost,
                       context_ptr->md_rate_estimation_ptr->switchable_restore_fac_bits,
                       3 * sizeof(int32_t));
            svt_memcpy(pcs_ptr->parent_pcs_ptr->av1x->wiener_restore_cost,
                       context_ptr->md_rate_estimation_ptr->wiener_restore_fac_bits,
                       2 * sizeof(int32_t));
            pcs_ptr->parent_pcs_ptr->av1x->rdmult =
                context_ptr->pic_full_lambda[(context_ptr->bit_depth == EB_10BIT) ? EB_10_BIT_MD
                                                                                  : EB_8_BIT_MD];
            svt_release_object(pcs_ptr->parent_pcs_ptr->me_data_wrapper_ptr);
            pcs_ptr->parent_pcs_ptr->me_data_wrapper_ptr = (EbObjectWrapper *)NULL;
            // Get Empty EncDec Results
            svt_get_empty_object(context_ptr->enc_dec_output_fifo_ptr, &enc_dec_results_wrapper_ptr);
            enc_dec_results_ptr = (EncDecResults *)enc_dec_results_wrapper_ptr->object_ptr;
            enc_dec_results_ptr->pcs_wrapper_ptr = enc_dec_tasks_ptr->pcs_wrapper_ptr;
            //CHKN these are not needed for DLF
            enc_dec_results_ptr->completed_sb_row_index_start = 0;
            enc_dec_results_ptr->completed_sb_row_count =
                ((pcs_ptr->parent_pcs_ptr->aligned_height + scs_ptr->sb_size_pix - 1) >> sb_size_log2);
            // Post EncDec Results
            svt_post_full_object(enc_dec_results_wrapper_ptr);
            }
        }
        }
        // Release Mode Decision Results
        svt_release_object(enc_dec_tasks_wrapper_ptr);
    }
    return NULL;
}

void svt_av1_add_film_grain(EbPictureBufferDesc *src, EbPictureBufferDesc *dst,
                            AomFilmGrain *film_grain_ptr) {
    uint8_t *luma, *cb, *cr;
    int32_t  height, width, luma_stride, chroma_stride;
    int32_t  use_high_bit_depth = 0;
    int32_t  chroma_subsamp_x   = 0;
    int32_t  chroma_subsamp_y   = 0;

    AomFilmGrain params = *film_grain_ptr;

    switch (src->bit_depth) {
    case EB_8BIT:
        params.bit_depth   = 8;
        use_high_bit_depth = 0;
        chroma_subsamp_x   = 1;
        chroma_subsamp_y   = 1;
        break;
    case EB_10BIT:
        params.bit_depth   = 10;
        use_high_bit_depth = 1;
        chroma_subsamp_x   = 1;
        chroma_subsamp_y   = 1;
        break;
    default: //todo: Throw an error if unknown format?
        params.bit_depth   = 10;
        use_high_bit_depth = 1;
        chroma_subsamp_x   = 1;
        chroma_subsamp_y   = 1;
    }

    dst->max_width  = src->max_width;
    dst->max_height = src->max_height;

    fgn_copy_rect(
        src->buffer_y + ((src->origin_y * src->stride_y + src->origin_x) << use_high_bit_depth),
        src->stride_y,
        dst->buffer_y + ((dst->origin_y * dst->stride_y + dst->origin_x) << use_high_bit_depth),
        dst->stride_y,
        dst->width,
        dst->height,
        use_high_bit_depth);

    fgn_copy_rect(src->buffer_cb + ((src->stride_cb * (src->origin_y >> chroma_subsamp_y) +
                                     (src->origin_x >> chroma_subsamp_x))
                                    << use_high_bit_depth),
                  src->stride_cb,
                  dst->buffer_cb + ((dst->stride_cb * (dst->origin_y >> chroma_subsamp_y) +
                                     (dst->origin_x >> chroma_subsamp_x))
                                    << use_high_bit_depth),
                  dst->stride_cb,
                  dst->width >> chroma_subsamp_x,
                  dst->height >> chroma_subsamp_y,
                  use_high_bit_depth);

    fgn_copy_rect(src->buffer_cr + ((src->stride_cr * (src->origin_y >> chroma_subsamp_y) +
                                     (src->origin_x >> chroma_subsamp_x))
                                    << use_high_bit_depth),
                  src->stride_cr,
                  dst->buffer_cr + ((dst->stride_cr * (dst->origin_y >> chroma_subsamp_y) +
                                     (dst->origin_x >> chroma_subsamp_x))
                                    << use_high_bit_depth),
                  dst->stride_cr,
                  dst->width >> chroma_subsamp_x,
                  dst->height >> chroma_subsamp_y,
                  use_high_bit_depth);

    luma = dst->buffer_y + ((dst->origin_y * dst->stride_y + dst->origin_x) << use_high_bit_depth);
    cb   = dst->buffer_cb + ((dst->stride_cb * (dst->origin_y >> chroma_subsamp_y) +
                            (dst->origin_x >> chroma_subsamp_x))
                           << use_high_bit_depth);
    cr   = dst->buffer_cr + ((dst->stride_cr * (dst->origin_y >> chroma_subsamp_y) +
                            (dst->origin_x >> chroma_subsamp_x))
                           << use_high_bit_depth);

    luma_stride   = dst->stride_y;
    chroma_stride = dst->stride_cb;

    width  = dst->width;
    height = dst->height;

    svt_av1_add_film_grain_run(&params,
                               luma,
                               cb,
                               cr,
                               height,
                               width,
                               luma_stride,
                               chroma_stride,
                               use_high_bit_depth,
                               chroma_subsamp_y,
                               chroma_subsamp_x);
    return;
}
