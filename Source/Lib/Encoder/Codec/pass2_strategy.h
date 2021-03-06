/*
 * Copyright (c) 2019, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#ifndef AOM_AV1_ENCODER_PASS2_STRATEGY_H_
#define AOM_AV1_ENCODER_PASS2_STRATEGY_H_

#include "level.h"
#include "encoder.h"
#include "Av1Common.h"

#ifdef __cplusplus
extern "C" {
#endif
#if FIX_KF_BOOST_CAP
#define FRAMES_TO_CHECK_DECAY 8
#define KF_MIN_FRAME_BOOST 80.0
#define KF_MAX_FRAME_BOOST 128.0
#define MIN_KF_BOOST 600  // Minimum boost for non-static KF interval
#define MAX_KF_BOOST 3200
#define MIN_STATIC_KF_BOOST 5400  // Minimum boost for static KF interval
#define MAX_KF_BOOST_LOW_KI 3000  // Maximum boost for KF with low interval
#define MAX_KF_BOOST_HIGHT_KI 5000  // Maximum boost for KF with hight interval
#define KF_INTERVAL_TH 64 // Low/high KF interval threshold
#endif
// structure of accumulated stats and features in a gf group
typedef struct {
    double gf_group_err;
    double gf_group_raw_error;
    double gf_group_skip_pct;
    double gf_group_inactive_zone_rows;

    double mv_ratio_accumulator;
    double decay_accumulator;
    double zero_motion_accumulator;
    double loop_decay_rate;
    double last_loop_decay_rate;
    double this_frame_mv_in_out;
    double mv_in_out_accumulator;
    double abs_mv_in_out_accumulator;

    double avg_sr_coded_error;
    double avg_tr_coded_error;
    double avg_pcnt_second_ref;
    double avg_pcnt_third_ref;
    double avg_pcnt_third_ref_nolast;
    double avg_new_mv_count;
#if !TUNE_FIRSTPASS_LOSSLESS
    double avg_wavelet_energy;
#endif
    double avg_raw_err_stdev;
    int    non_zero_stdev_count;
} GF_GROUP_STATS;

typedef struct {
    double frame_err;
    double frame_coded_error;
    double frame_sr_coded_error;
    double frame_tr_coded_error;
} GF_FRAME_STATS;

void svt_av1_init_second_pass(struct SequenceControlSet *scs_ptr);
void svt_av1_init_single_pass_lap(struct SequenceControlSet *scs_ptr);
void svt_av1_new_framerate(struct SequenceControlSet *scs_ptr, double framerate);

void svt_av1_get_second_pass_params(struct PictureParentControlSet *pcs_ptr);

void svt_av1_twopass_postencode_update(struct PictureParentControlSet *ppcs_ptr);

int frame_is_kf_gf_arf(PictureParentControlSet *ppcs_ptr);
#ifdef __cplusplus
} // extern "C"
#endif

#endif // AOM_AV1_ENCODER_PASS2_STRATEGY_H_
