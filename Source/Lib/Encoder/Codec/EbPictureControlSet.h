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

#ifndef EbPictureControlSet_h
#define EbPictureControlSet_h

#include "EbSvtAv1Enc.h"
#include "EbDefinitions.h"
#include "EbSystemResourceManager.h"
#include "EbPictureBufferDesc.h"
#include "EbCodingUnit.h"
#include "EbEntropyCodingObject.h"
#include "EbDefinitions.h"
#include "EbPredictionStructure.h"
#include "EbNeighborArrays.h"
#include "EbModeDecisionSegments.h"
#include "EbEncDecSegments.h"
#if !CLN_OLD_RC
#include "EbRateControlTables.h"
#endif
#include "EbRestoration.h"
#include "EbObject.h"
#include "noise_model.h"
#include "EbSegmentationParams.h"
#include "EbAv1Structs.h"
#include "EbMdRateEstimation.h"
#include "EbEncCdef.h"
#include "Av1Common.h"

#include "av1me.h"
#include "hash_motion.h"
#include "firstpass.h"

#ifdef __cplusplus
extern "C" {
#endif
#define HISTOGRAM_NUMBER_OF_BINS 256
#define MAX_NUMBER_OF_REGIONS_IN_WIDTH 4
#define MAX_NUMBER_OF_REGIONS_IN_HEIGHT 4
#define MAX_REF_QP_NUM 81
#define QPS_SW_THRESH 8 // 100 to shut QPS/QPM (i.e. CORE only)
// BDP OFF
#define MD_NEIGHBOR_ARRAY_INDEX 0
#define MULTI_STAGE_PD_NEIGHBOR_ARRAY_INDEX 4
#define NEIGHBOR_ARRAY_TOTAL_COUNT 5
#define AOM_QM_BITS 5

typedef struct DepCntPicInfo {
    uint64_t pic_num;
    int32_t
        dep_cnt_diff; //increase(e.g 4L->5L) or decrease of dep cnt . not including the run-time decrease
} DepCntPicInfo;
typedef struct EbDownScaledBufDescPtrArray {
    EbPictureBufferDesc *picture_ptr;
    EbPictureBufferDesc *quarter_picture_ptr;
    EbPictureBufferDesc *sixteenth_picture_ptr;
    uint64_t             picture_number;
} EbDownScaledBufDescPtrArray;

typedef struct EbDownScaledObject {
    EbDctor dctor;
    //EbPictureBufferDesc *picture_ptr; // original picture, just a pointer, don't allocate resource here
    EbPictureBufferDesc *quarter_picture_ptr;
    EbPictureBufferDesc *sixteenth_picture_ptr;
    //uint64_t            picture_number;
} EbDownScaledObject;

typedef struct EbDownScaledObjectDescInitData {
    EbPictureBufferDescInitData quarter_picture_desc_init_data;
    EbPictureBufferDescInitData sixteenth_picture_desc_init_data;

    // whether enable 1/4,1/16 8bit luma for in_loop global motion
    uint8_t enable_quarter_luma_input;
    uint8_t enable_sixteenth_luma_input;
} EbDownScaledObjectDescInitData;
typedef struct MacroblockPlane {
    // Quantizer setings
    // These are used/accessed only in the quantization process
    // RDO does not / must not depend on any of these values
    // All values below share the coefficient scale/shift used in TX
    const int16_t *quant_fp_qtx;
    const int16_t *round_fp_qtx;
    const int16_t *quant_qtx;
    const int16_t *quant_shift_qtx;
    const int16_t *zbin_qtx;
    const int16_t *round_qtx;
    const int16_t *dequant_qtx;
} MacroblockPlane;
// The Quants structure is used only for internal quantizer setup in
// av1_quantize.c.
// All of its fields use the same coefficient shift/scaling at TX.
typedef struct Quants {
    // 0: dc 1: ac 2-8: ac repeated to SIMD width
    DECLARE_ALIGNED(16, int16_t, y_quant[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, y_quant_shift[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, y_zbin[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, y_round[QINDEX_RANGE][8]);

    // if we want to deprecate the current use of y_quant.
    DECLARE_ALIGNED(16, int16_t, y_quant_fp[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, u_quant_fp[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, v_quant_fp[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, y_round_fp[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, u_round_fp[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, v_round_fp[QINDEX_RANGE][8]);

    DECLARE_ALIGNED(16, int16_t, u_quant[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, v_quant[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, u_quant_shift[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, v_quant_shift[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, u_zbin[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, v_zbin[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, u_round[QINDEX_RANGE][8]);
    DECLARE_ALIGNED(16, int16_t, v_round[QINDEX_RANGE][8]);
} Quants;

// The Dequants structure is used only for internal quantizer setup in
// av1_quantize.c.
// Fields are sufffixed according to whether or not they're expressed in
// the same coefficient shift/precision as TX or a fixed Q3 format.
typedef struct Dequants {
    DECLARE_ALIGNED(16, int16_t,
                    y_dequant_qtx[QINDEX_RANGE][8]); // 8: SIMD width
    DECLARE_ALIGNED(16, int16_t,
                    u_dequant_qtx[QINDEX_RANGE][8]); // 8: SIMD width
    DECLARE_ALIGNED(16, int16_t,
                    v_dequant_qtx[QINDEX_RANGE][8]); // 8: SIMD width
    DECLARE_ALIGNED(16, int16_t, y_dequant_q3[QINDEX_RANGE][8]); // 8: SIMD width
    DECLARE_ALIGNED(16, int16_t, u_dequant_q3[QINDEX_RANGE][8]); // 8: SIMD width
    DECLARE_ALIGNED(16, int16_t, v_dequant_q3[QINDEX_RANGE][8]); // 8: SIMD width
} Dequants;

typedef struct MacroblockdPlane {
    //TranLow *dqcoeff;
    PlaneType    plane_type;
    int32_t      subsampling_x;
    int32_t      subsampling_y;
    struct Buf2D dst;
    int32_t      is_16bit;
    //struct Buf2D pre[2];
    //EntropyContext *above_context;
    //EntropyContext *left_context;
    // The dequantizers below are true dequntizers used only in the
    // dequantization process.  They have the same coefficient
    // shift/scale as TX.
    //int16_t seg_dequant_qtx[MAX_SEGMENTS][2];
    //uint8_t *color_index_map;
    // block size in pixels
    //uint8_t width, height;
    //QmVal *seg_iqmatrix[MAX_SEGMENTS][TX_SIZES_ALL];
    //QmVal *seg_qmatrix[MAX_SEGMENTS][TX_SIZES_ALL];
    // the 'dequantizers' below are not literal dequantizer values.
    // They're used by encoder RDO to generate ad-hoc lambda values.
    // They use a hardwired Q3 coeff shift and do not necessarily match
    // the TX scale in use.
    //const int16_t *dequant_Q3;
} MacroblockdPlane;

struct PredictionUnit;

/**************************************
     * Segment-based Control Sets
     **************************************/

typedef struct EbMdcLeafData {
    uint32_t mds_idx;
    uint32_t tot_d1_blocks; //how many d1 bloks every parent square would have
#if !OPT6_DEPTH_REFINEMENT
    EbBool   split_flag;
    uint8_t  consider_block;
    uint8_t  refined_split_flag;
#endif
#if !OPT_REFINEMENT_SIGNALS
    int8_t   pred_depth_refinement;
    int8_t   final_pred_depth_refinement;
    int8_t   pred_depth;
    int8_t   final_pred_depth;
#endif
} EbMdcLeafData;

typedef struct MdcSbData {
    uint32_t      leaf_count;
    EbMdcLeafData leaf_data_array[BLOCK_MAX_COUNT_SB_128];
#if OPT6_DEPTH_REFINEMENT
    EbBool   split_flag[BLOCK_MAX_COUNT_SB_128];
    uint8_t  consider_block[BLOCK_MAX_COUNT_SB_128];
    uint8_t  refined_split_flag[BLOCK_MAX_COUNT_SB_128];
#endif
} MdcSbData;

/**************************************
     * MD Segment Control
     **************************************/
typedef struct MdSegmentCtrl {
    uint64_t completion_mask;
    EbHandle write_lock_mutex;
    uint32_t total_count;
    uint32_t column_count;
    uint32_t row_count;
    EbBool   in_progress;
    uint32_t current_row_idx;
} MdSegmentCtrl;

/**************************************
     * Picture Control Set
     **************************************/
struct CodedTreeblock_s;
struct SuperBlock;
#define MAX_MESH_STEP 4

typedef struct MeshPattern {
    int range;
    int interval;
} MeshPattern;

typedef struct CdfControls {
    uint8_t enabled; //1 if mv, or se, or coeff is ON
    uint8_t update_mv; //cdf update for mv
    uint8_t update_se; //cdf update for various syntax elements
    uint8_t update_coef; //cdf update for coeffs
} CdfControls;
typedef struct SpeedFeatures {
    // This allows us to use motion search at other sizes as a starting
    // point for this motion search and limits the search range around it.
    int adaptive_motion_search;

    // Flag for allowing some use of exhaustive searches;
    int allow_exhaustive_searches;

    // Threshold for allowing exhaistive motion search.
    int exhaustive_searches_thresh;

    // Maximum number of exhaustive searches for a frame.
    int max_exaustive_pct;

    // Pattern to be used for any exhaustive mesh searches.
    MeshPattern mesh_patterns[MAX_MESH_STEP];

} SpeedFeatures;
#if CLN_STRUCT
typedef struct EncDecSet {
    EbDctor          dctor;
    EbPictureBufferDesc *recon_picture_ptr;
    EbPictureBufferDesc *recon_picture16bit_ptr;
    EbPictureBufferDesc **quantized_coeff;
    EbObjectWrapper *enc_dec_wrapper_ptr;
    struct PictureParentControlSet *parent_pcs_ptr; //The parent of this PCS.
    EbObjectWrapper *               picture_parent_control_set_wrapper_ptr;

    uint16_t sb_total_count_unscaled;

} EncDecSet;
#endif
typedef struct PictureControlSet {
    /*!< Pointer to the dtor of the struct*/
    EbDctor          dctor;
    EbObjectWrapper *scs_wrapper_ptr;
#if !CLN_STRUCT
    EbPictureBufferDesc *recon_picture_ptr;
#endif
    EbPictureBufferDesc *film_grain_picture_ptr;
#if !CLN_STRUCT
    EbPictureBufferDesc *recon_picture16bit_ptr;
#endif
    EbPictureBufferDesc *film_grain_picture16bit_ptr;
    EbPictureBufferDesc *input_frame16bit;

    struct PictureParentControlSet *parent_pcs_ptr; //The parent of this PCS.
    EbObjectWrapper *               picture_parent_control_set_wrapper_ptr;
    // Packetization (used to encode SPS, PPS, etc)
    Bitstream *bitstream_ptr;

    EbObjectWrapper *c_pcs_wrapper_ptr;

    // Reference Lists
    // Reference Lists
    EbObjectWrapper *ref_pic_ptr_array[MAX_NUM_OF_REF_PIC_LIST][REF_LIST_MAX_DEPTH];
    //EB_S64                                refPicPocArray[MAX_NUM_OF_REF_PIC_LIST][REF_LIST_MAX_DEPTH];

    uint8_t  ref_pic_qp_array[MAX_NUM_OF_REF_PIC_LIST][REF_LIST_MAX_DEPTH];
    EB_SLICE ref_slice_type_array[MAX_NUM_OF_REF_PIC_LIST][REF_LIST_MAX_DEPTH];
    uint64_t ref_pic_referenced_area_avg_array[MAX_NUM_OF_REF_PIC_LIST][REF_LIST_MAX_DEPTH];
    double   ref_pic_r0[MAX_NUM_OF_REF_PIC_LIST][REF_LIST_MAX_DEPTH];
    // GOP
    uint64_t         picture_number;
    uint8_t          temporal_layer_index;
    EbColorFormat    color_format;
    EncDecSegments **enc_dec_segment_ctrl;
    uint16_t         enc_dec_coded_sb_count;

    // Entropy Process Rows
    EntropyTileInfo **entropy_coding_info;
    EbHandle          entropy_coding_pic_mutex;
    EbBool            entropy_coding_pic_reset_flag;
    uint8_t           tile_size_bytes_minus_1;
    EbHandle          intra_mutex;
#if !TUNE_REMOVE_INTRA_STATS_TRACKING
    uint32_t          intra_coded_area;
#endif
    uint32_t          tot_seg_searched_cdef;
    EbHandle          cdef_search_mutex;

    uint16_t cdef_segments_total_count;
    uint8_t  cdef_segments_column_count;
    uint8_t  cdef_segments_row_count;

    uint64_t (*mse_seg[2])[TOTAL_STRENGTHS];

    uint16_t *src[3]; //dlfed recon in 16bit form
    uint16_t *ref_coeff[3]; //input video in 16bit form

    uint32_t tot_seg_searched_rest;
    EbHandle rest_search_mutex;
    uint16_t rest_segments_total_count;
    uint8_t  rest_segments_column_count;
    uint8_t  rest_segments_row_count;

    // Slice Type
    EB_SLICE slice_type;

    // Rate Control
    uint8_t picture_qp;
    uint8_t dif_blk_delta_qp_depth;

    // SB Array
    uint16_t     sb_total_count;
    SuperBlock **sb_ptr_array;

    // Mode Decision Neighbor Arrays
    NeighborArrayUnit **md_intra_luma_mode_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
#if !CLN_MDC_CTX
    NeighborArrayUnit **md_intra_chroma_mode_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_mv_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
#endif
    NeighborArrayUnit **md_skip_flag_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_mode_type_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_luma_recon_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_tx_depth_1_luma_recon_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_tx_depth_2_luma_recon_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_cb_recon_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_cr_recon_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];

    uint8_t             hbd_mode_decision;
    NeighborArrayUnit **md_luma_recon_neighbor_array16bit[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_tx_depth_1_luma_recon_neighbor_array16bit[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_tx_depth_2_luma_recon_neighbor_array16bit[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_cb_recon_neighbor_array16bit[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_cr_recon_neighbor_array16bit[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_luma_dc_sign_level_coeff_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit *
        *md_tx_depth_1_luma_dc_sign_level_coeff_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_cb_dc_sign_level_coeff_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_cr_dc_sign_level_coeff_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
    NeighborArrayUnit **md_txfm_context_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
#if !CLN_MDC_CTX
    NeighborArrayUnit **md_inter_pred_dir_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];
#endif
    NeighborArrayUnit **md_ref_frame_type_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];

    NeighborArrayUnit32 **md_interpolation_type_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];

    NeighborArrayUnit **mdleaf_partition_neighbor_array[NEIGHBOR_ARRAY_TOTAL_COUNT];

    // Encode Pass Neighbor Arrays
    NeighborArrayUnit **ep_intra_luma_mode_neighbor_array;
    NeighborArrayUnit **ep_intra_chroma_mode_neighbor_array;
    NeighborArrayUnit **ep_mv_neighbor_array;
    NeighborArrayUnit **ep_skip_flag_neighbor_array;
    NeighborArrayUnit **ep_mode_type_neighbor_array;
    NeighborArrayUnit **ep_luma_recon_neighbor_array;
    NeighborArrayUnit **ep_cb_recon_neighbor_array;
    NeighborArrayUnit **ep_cr_recon_neighbor_array;
    NeighborArrayUnit **ep_luma_recon_neighbor_array16bit;
    NeighborArrayUnit **ep_cb_recon_neighbor_array16bit;
    NeighborArrayUnit **ep_cr_recon_neighbor_array16bit;
    NeighborArrayUnit **ep_luma_dc_sign_level_coeff_neighbor_array;
    NeighborArrayUnit **ep_cr_dc_sign_level_coeff_neighbor_array;
    NeighborArrayUnit **ep_cb_dc_sign_level_coeff_neighbor_array;
    NeighborArrayUnit **ep_partition_context_neighbor_array;

    // Entropy Coding Neighbor Arrays
    NeighborArrayUnit **mode_type_neighbor_array;
    NeighborArrayUnit **partition_context_neighbor_array;
    NeighborArrayUnit **intra_luma_mode_neighbor_array;
    NeighborArrayUnit **skip_flag_neighbor_array;
    NeighborArrayUnit **skip_coeff_neighbor_array;
    NeighborArrayUnit **
        luma_dc_sign_level_coeff_neighbor_array; // Stored per 4x4. 8 bit: lower 6 bits (COEFF_CONTEXT_BITS), shows if there is at least one Coef. Top 2 bit store the sign of DC as follow: 0->0,1->-1,2-> 1
    NeighborArrayUnit **
        cr_dc_sign_level_coeff_neighbor_array; // Stored per 4x4. 8 bit: lower 6 bits(COEFF_CONTEXT_BITS), shows if there is at least one Coef. Top 2 bit store the sign of DC as follow: 0->0,1->-1,2-> 1
    NeighborArrayUnit **
                          cb_dc_sign_level_coeff_neighbor_array; // Stored per 4x4. 8 bit: lower 6 bits(COEFF_CONTEXT_BITS), shows if there is at least one Coef. Top 2 bit store the sign of DC as follow: 0->0,1->-1,2-> 1
    NeighborArrayUnit **  txfm_context_array;
#if !CLN_MDC_CTX
    NeighborArrayUnit **  inter_pred_dir_neighbor_array;
#endif
    NeighborArrayUnit **  ref_frame_type_neighbor_array;
    NeighborArrayUnit32 **interpolation_type_neighbor_array;

    NeighborArrayUnit **     segmentation_id_pred_array;
    SegmentationNeighborMap *segmentation_neighbor_map;

    ModeInfo **mi_grid_base; //2 SB Rows of mi Data are enough

    ModeInfo *mip;

    int32_t          mi_stride;
    EbReflist        colocated_pu_ref_list;
    EbEncMode        enc_mode;
    int32_t          cdef_preset[MAX_TILE_CNTS][4];
    WienerInfo       wiener_info[MAX_TILE_CNTS][MAX_MB_PLANE];
    SgrprojInfo      sgrproj_info[MAX_TILE_CNTS][MAX_MB_PLANE];
    SpeedFeatures    sf;
    SearchSiteConfig ss_cfg; //CHKN this might be a seq based
    HashTable        hash_table;
    CRC_CALCULATOR   crc_calculator1;
    CRC_CALCULATOR   crc_calculator2;

    FRAME_CONTEXT *                 ec_ctx_array;
    FRAME_CONTEXT                   md_frame_context;
    CdfControls                     cdf_ctrl;
    FRAME_CONTEXT                   ref_frame_context[REF_FRAMES];
    EbWarpedMotionParams            ref_global_motion[TOTAL_REFS_PER_FRAME];
    struct MdRateEstimationContext *md_rate_estimation_array;
    int8_t                          ref_frame_side[REF_FRAMES];
    TPL_MV_REF *                    tpl_mvs;
    uint8_t                         pic_filter_intra_level;
    TOKENEXTRA *                    tile_tok[64][64];
    //Put it here for deinit, don't need to go pcs->ppcs->av1_cm which may already be released
    uint16_t tile_row_count;
    uint16_t tile_column_count;
    uint16_t sb_total_count_pix;
    uint16_t sb_total_count_unscaled;
    // pointer to a scratch buffer used by self-guided restoration
    int32_t *rst_tmpbuf;
#if !CLN_REMOVE_UNUSED_CODE
    uint32_t part_cnt[NUMBER_OF_SHAPES - 1][FB_NUM][SSEG_NUM];
#endif
#if !CLN_NSQ_AND_STATS
    uint32_t pred_depth_count[DEPTH_DELTA_NUM][NUMBER_OF_SHAPES - 1];
#endif
#if !TUNE_REMOVE_TXT_STATS
    uint32_t txt_cnt[TXT_DEPTH_DELTA_NUM][TX_TYPES];
#endif

#if CLN_DLF_RES_PROCESS
    EbPictureBufferDesc *temp_lf_recon_picture_ptr;
    EbPictureBufferDesc *temp_lf_recon_picture16bit_ptr;
#endif

#if CLN_RES_PROCESS
    RestUnitSearchInfo *rusi_picture[3]; //for 3 planes
#endif

#if CLN_BN
    RestorationInfo rst_info[MAX_MB_PLANE];
    // rst_end_stripe[i] is one more than the index of the bottom stripe
    // for tile row i.
    int32_t rst_end_stripe[MAX_TILE_ROWS];
#endif
} PictureControlSet;

// To optimize based on the max input size
// To study speed-memory trade-offs
typedef struct SbParams {
    uint8_t  horizontal_index;
    uint8_t  vertical_index;
    uint16_t origin_x;
    uint16_t origin_y;
    uint8_t  width;
    uint8_t  height;
    uint8_t  is_complete_sb;
    EbBool   raster_scan_blk_validity[CU_MAX_COUNT];
    uint8_t  is_edge_sb;
    uint32_t tile_start_x;
    uint32_t tile_start_y;
    uint32_t tile_end_x;
    uint32_t tile_end_y;
} SbParams;

typedef struct SbGeom {
    uint16_t horizontal_index;
    uint16_t vertical_index;
    uint16_t origin_x;
    uint16_t origin_y;
    uint8_t  width;
    uint8_t  height;
    uint8_t  is_complete_sb;
    EbBool   block_is_inside_md_scan[BLOCK_MAX_COUNT_SB_128];
    EbBool   block_is_allowed[BLOCK_MAX_COUNT_SB_128];
} SbGeom;

typedef struct TileGroupInfo {
    uint16_t tile_group_sb_start_x;
    uint16_t tile_group_sb_start_y;
    uint16_t tile_group_sb_end_x;
    uint16_t tile_group_sb_end_y;
    uint16_t tile_group_width_in_sb;
    uint16_t tile_group_height_in_sb;

    uint16_t tile_group_tile_start_x;
    uint16_t tile_group_tile_start_y;
    uint16_t tile_group_tile_end_x;
    uint16_t tile_group_tile_end_y;
} TileGroupInfo;
typedef struct MotionEstimationData {
    EbDctor       dctor;
    MeSbResults **me_results;
    uint16_t      sb_total_count_unscaled;
#if OPT_ME
    uint8_t       max_cand;//total max me candidates given the active references
    uint8_t       max_refs;//total max active references
    uint8_t       max_l0;  //max active refs in L0
#endif
} MotionEstimationData;
typedef struct TplControls {
    uint8_t tpl_opt_flag; // 0:OFF 1:ON - TPL optimizations : no rate, only DC
    uint8_t enable_tpl_qps; // 0:OFF 1:ON - QPS in TPL
    uint8_t disable_intra_pred_nref; // 0:OFF 1:ON - Disable intra prediction in NREF
    uint8_t disable_intra_pred_nbase; // 0:OFF 1:ON - Disable intra prediction in NBASE
    uint8_t disable_tpl_nref; // 0:OFF 1:ON - Disable tpl in NREF
    uint8_t disable_tpl_pic_dist; // 16: OFF - 0: ON
    uint8_t get_best_ref; // Reference pruning, get best reference
#if OPT_TPL
    EB_TRANS_COEFF_SHAPE pf_shape;
    uint8_t use_pred_sad_in_intra_search;
#endif
#if FTR_TPL_REDUCE_NUMBER_OF_REF
    uint8_t use_pred_sad_in_inter_search;
#endif
#if FTR_USE_LAD_TPL
    uint8_t reduced_tpl_group;
#endif
#if FTR_BYPASS_RDOQ_CHROMA_QP_BASED
    uint8_t skip_rdoq_uv_qp_based_th;
#endif
#if FIX_SCD
    double r0_adjust_factor;
#endif
} TplControls;

/*!
 * \brief Refresh frame flags for different type of frames.
 *
 * If the refresh flag is true for a particular reference frame, after the
 * current frame is encoded, the reference frame gets refreshed (updated) to
 * be the current frame. Note: Usually at most one flag will be set to true at
 * a time. But, for key-frames, all flags are set to true at once.
 */
typedef struct {
    bool golden_frame; /*!< Refresh flag for golden frame */
    bool bwd_ref_frame; /*!< Refresh flag for bwd-ref frame */
    bool alt_ref_frame; /*!< Refresh flag for alt-ref frame */
} RefreshFrameFlagsInfo;

typedef struct {
    uint8_t                     tpl_temporal_layer_index;
    EB_SLICE                    tpl_slice_type;
    uint8_t                     tpl_ref0_count;
    uint8_t                     tpl_ref1_count;
    uint64_t                    tpl_decode_order;
    EbBool                      ref_in_slide_window[MAX_NUM_OF_REF_PIC_LIST][REF_LIST_MAX_DEPTH];
    EbBool                      is_used_as_reference_flag;
    EbDownScaledBufDescPtrArray tpl_ref_ds_ptr_array[MAX_NUM_OF_REF_PIC_LIST][REF_LIST_MAX_DEPTH];
#if !FTR_TPL_TR
    TplControls                 tpl_ctrls;
#endif
} TPLData;
#if !TUNE_REDESIGN_TF_CTRLS
typedef struct TfControls {
    uint8_t enabled;
    uint8_t window_size; // 3, 5, 7
#if TUNE_FIX_TF
    uint8_t adjust_num_level; // 0: no adjustment (final_window_size = window_size), 1/2/3: final_window_size = window_size + adjust_num and adjust_num = f(noise_level)
#else
    uint8_t
             noise_based_window_adjust; // add an offset to default window_size based on the noise level; higher the noise, smaller is the offset
#endif
    uint8_t  hp; // w/o 1/16 pel MV refinement
    uint8_t  chroma; // use chroma
    uint64_t block_32x32_16x16_th; // control tf_16x16 using tf_32x32 pred error
#if FTR_OPTIMISE_TF
    uint8_t bypass_halfpel; // bypass 1/2-pel search
#endif
} TfControls;
#endif
typedef struct GmControls {
    uint8_t enabled;
    uint8_t
            identiy_exit; // 0: generate GM params for both list_0 and list_1, 1: do not generate GM params for list_1 if list_0/ref_idx_0 is id
    uint8_t rotzoom_model_only; // 0: use both rotzoom and affine models, 1:use rotzoom model only
    uint8_t bipred_only; // 0: test both unipred and bipred, 1: test bipred only
#if FTR_GM_OPT_BASED_ON_ME
    uint8_t bypass_based_on_me;
#if TUNE_M9_GM_DETECTOR
    uint8_t use_stationary_block; // 0: do not consider stationary_block info @ me-based bypass, 1: consider stationary_block info @ me-based bypass (only if bypass_based_on_me=1)
    uint8_t use_distance_based_active_th; // 0: used default active_th,1: increase active_th baed on distance to ref (only if bypass_based_on_me=1)
#endif
#endif
} GmControls;
#if FTR_TPL_TR
/* a local PCS wraper to make TPL : PCS agnostic*/
typedef struct {
    TPLData          tpl_data;
    uint64_t         picture_number;
    TplStats       **tpl_stats;
    EB_SLICE         slice_type;
    EbPictureBufferDesc *enhanced_picture_ptr;
    uint8_t          hierarchical_levels;
    uint16_t         sb_total_count;
    OisMbResults   **ois_mb_results;
    struct SequenceControlSet *scs_ptr;
    uint8_t           max_number_of_pus_per_sb;
    MotionEstimationData *pa_me_data;
    Av1Common *                av1_cm;
    int32_t                    is_720p_or_larger;
    uint16_t                   aligned_width;

#if FTR_TPL_TR
    TplControls  tpl_ctrls;
#endif
#if TPL_KERNEL

   // EbObjectWrapper *       p_pcs_wrapper_ptr;
    EbHandle                tpl_disp_done_semaphore;
#endif
#if TPL_SEG

    uint16_t tpl_disp_coded_sb_count;

    EncDecSegments **tpl_disp_segment_ctrl;

    TileGroupInfo *     tile_group_info;

    EbHandle tpl_disp_mutex;
#endif
} TplPcs;
#endif
#if TPL_KERNEL


typedef struct TplDispResults {
    EbDctor          dctor;
    EbObjectWrapper *pcs_wrapper_ptr;
    uint32_t        frame_index;
#if !TPL_SEG
    uint32_t        sb_index;
#endif
#if TPL_SEG
    EbFifo *sbo_feedback_fifo_ptr;
    uint32_t  input_type;
    int16_t   enc_dec_segment_row;
    uint16_t  tile_group_index;
#endif
#if TPL_KERNEL
    TplPcs* pcs_ptr;
    int32_t         qIndex;

#endif
} TplDispResults;

#endif
#if FTR_TPL_TR
/* a local PCS wraper to make ME : PCS agnostic*/
typedef struct MePcs {

    uint64_t                   picture_number;
    uint16_t                   sb_total_count;
    uint32_t                   temporal_layer_index;
    uint8_t                    picture_qp;
    uint8_t                    max_number_of_pus_per_sb;
    uint32_t                  *rc_me_distortion;
#if FTR_GM_OPT_BASED_ON_ME
#if TUNE_M9_GM_DETECTOR
    uint8_t *     stationary_block_present_sb; // 1 when a % of the SB is stationary relative to reference frame(s) ((0,0) MV: decode order), 0 otherwise
#endif
    uint8_t *     rc_me_allow_gm;
#endif
#if FTR_EARLY_DEPTH_REMOVAL
    uint32_t                  *me_64x64_distortion;
    uint32_t                  *me_32x32_distortion;
    uint32_t                  *me_16x16_distortion;
    uint32_t                  *me_8x8_distortion;
#endif
#if TUNE_DEPTH_REMOVAL_PER_RESOLUTION
    uint32_t *me_8x8_cost_variance;
#endif
    SbParams                  *sb_params_array;
    uint16_t                   aligned_width;
    uint16_t                   aligned_height;
    MotionEstimationData      *pa_me_data;
    EbPictureBufferDesc       *enhanced_picture_ptr;
    OisMbResults             **ois_mb_results;
    struct SequenceControlSet *scs_ptr;
    //TPLData                    tpl_data;
#if FTR_TPL_TR
    TplControls                tpl_ctrls;
#endif
#if TUNE_M9_GM_DETECTOR
    GmControls                 gm_ctrls;
#endif
    EbEncMode                  enc_mode;
    EbBool                     enable_hme_flag;
    EbBool                     enable_hme_level0_flag;
    EbBool                     enable_hme_level1_flag;
    EbBool                     enable_hme_level2_flag;
#if FTR_ALIGN_SC_DETECOR
    uint8_t                    sc_class0;
    uint8_t                    sc_class1;
    uint8_t                    sc_class2;
#else
    uint8_t                    sc_content_detected;
#endif
    EbObjectWrapper           *pa_reference_picture_wrapper_ptr;
    EbPictureBufferDesc       *enhanced_unscaled_picture_ptr;

    uint16_t                   me_segments_total_count;
    uint8_t                    me_segments_column_count;
    uint8_t                    me_segments_row_count;

    EB_SLICE                   slice_type;

} MePcs;
#endif
//CHKN
// Add the concept of PictureParentControlSet which is a subset of the old PictureControlSet.
// It actually holds only high level Picture based control data:(GOP management,when to start a picture, when to release the PCS, ....).
// The regular PictureControlSet(Child) will be dedicated to store SB based encoding results and information.
// Parent is created before the Child, and continue to live more. Child PCS only lives the exact time needed to encode the picture: from ME to EC/ALF.
typedef struct PictureParentControlSet {
    EbDctor              dctor;
    EbObjectWrapper *    scs_wrapper_ptr;
    EbObjectWrapper *    input_picture_wrapper_ptr;
    EbObjectWrapper *    reference_picture_wrapper_ptr;
    EbObjectWrapper *    pa_reference_picture_wrapper_ptr;
    EbPictureBufferDesc *enhanced_picture_ptr;
    EbPictureBufferDesc *enhanced_downscaled_picture_ptr;
    EbPictureBufferDesc *enhanced_unscaled_picture_ptr;
    EbPictureBufferDesc
        *  chroma_downsampled_picture_ptr; //if 422/444 input, down sample to 420 for MD
    EbBool is_chroma_downsampled_picture_ptr_owner;
    PredictionStructure *      pred_struct_ptr; // need to check
    struct SequenceControlSet *scs_ptr;
    EbObjectWrapper *          p_pcs_wrapper_ptr;
    EbObjectWrapper *          previous_picture_control_set_wrapper_ptr;
    EbObjectWrapper *          output_stream_wrapper_ptr;
    Av1Common *                av1_cm;

#if FIX_R2R_10B_LAMBDA
    uint8_t             hbd_mode_decision;
#endif
    // Data attached to the picture. This includes data passed from the application, or other data the encoder attaches
    // to the picture.
    EbLinkedListNode *data_ll_head_ptr;
    // pointer to data to be passed back to the application when picture encoding is done
    EbLinkedListNode *app_out_data_ll_head_ptr;

    EbBufferHeaderType *input_ptr; // input picture buffer
    uint8_t             log2_tile_rows;
    uint8_t             log2_tile_cols;
    uint8_t             log2_sb_sz;
    TileGroupInfo *     tile_group_info;
#if TPL_KERNEL
    TileGroupInfo *     tile_group_info_trail;
#endif
    uint8_t             tile_group_cols;
    uint8_t             tile_group_rows;

    EbBool          idr_flag;
    EbBool          cra_flag;
    EbBool          scene_change_flag;
    EbBool          end_of_sequence_flag;
    uint8_t         picture_qp;
    uint64_t        picture_number;
#if TUNE_FIRSTPASS_SKIP_FRAME
    uint8_t         skip_frame;
#endif
    uint32_t        cur_order_hint;
    uint32_t        ref_order_hint[7];
#if !OPT1_REMOVE_FLAT_NOISE
    EbPicnoiseClass pic_noise_class;
#endif
    EB_SLICE        slice_type;
    uint8_t         pred_struct_index;
    uint8_t         temporal_layer_index;
    uint64_t        decode_order;
    EbBool          is_used_as_reference_flag;
    uint8_t         ref_list0_count;
    uint8_t         ref_list1_count;
    uint8_t
        ref_list0_count_try; //The number of references to try (in ME / MD) in list0.Should be <= ref_list0_count.
    uint8_t
                     ref_list1_count_try; // The number of references to try (in ME/MD) in list1. Should be <= ref_list1_count.
    MvReferenceFrame ref_frame_type_arr[MODE_CTX_REF_FRAMES];
    uint8_t          tot_ref_frame_types;
    // Rate Control
#if !CLN_OLD_RC
    uint64_t pred_bits_ref_qp[MAX_REF_QP_NUM];
    uint64_t target_bits_best_pred_qp;
    uint64_t target_bits_rc;
    uint8_t  best_pred_qp;
#endif
    uint64_t total_num_bits;
#if !CLN_OLD_RC
    uint8_t  first_frame_in_temporal_layer;
    uint8_t  first_non_intra_frame_in_temporal_layer;
    uint64_t frames_in_interval[EB_MAX_TEMPORAL_LAYERS];
    uint64_t bits_per_sw_per_layer[EB_MAX_TEMPORAL_LAYERS];
    uint64_t total_bits_per_gop;
    EbBool   tables_updated;
    EbBool   percentage_updated;
    uint32_t target_bit_rate;
    uint32_t vbv_bufsize;
    uint32_t frame_rate;
#endif
    uint16_t sb_total_count;
    EbBool   end_of_sequence_region;
#if !CLN_OLD_RC
    EbBool   scene_change_in_gop;
#endif
    uint8_t  frames_in_sw; // used for Look ahead
#if !CLN_OLD_RC
    int8_t   historgram_life_count;
#endif
    EbBool   qp_on_the_fly;
    uint8_t  calculated_qp;
    uint8_t  intra_selected_org_qp;
    uint64_t sad_me;
    uint64_t quantized_coeff_num_bits;
    uint64_t average_qp;
    uint64_t last_idr_picture;
    uint64_t start_time_seconds;
    uint64_t start_time_u_seconds;
    uint32_t luma_sse;
    uint32_t cr_sse;
    uint32_t cb_sse;
    double   luma_ssim;
    double   cr_ssim;
    double   cb_ssim;

    EbObjectWrapper *           down_scaled_picture_wrapper_ptr;
    EbDownScaledBufDescPtrArray ds_pics; // Pointer array for down scaled pictures

    TPLData tpl_data;

    // Pre Analysis
    EbObjectWrapper *ref_pa_pic_ptr_array[MAX_NUM_OF_REF_PIC_LIST][REF_LIST_MAX_DEPTH];
    uint64_t         ref_pic_poc_array[MAX_NUM_OF_REF_PIC_LIST][REF_LIST_MAX_DEPTH];
    uint16_t **      variance;
#if !CLN_REMOVE_MEAN
    uint8_t **       y_mean;
    uint8_t **       cb_mean;
    uint8_t **       cr_mean;
#endif
    uint32_t         pre_assignment_buffer_count;
    uint16_t         pic_avg_variance;
    EbBool           scene_transition_flag[MAX_NUM_OF_REF_PIC_LIST];
    uint8_t          average_intensity[3];
    // Non moving index array
    uint8_t *non_moving_index_array;
    int      kf_zeromotion_pct; // percent of zero motion blocks
    uint8_t  fade_out_from_black;
    uint8_t  fade_in_to_black;
#if !OPT1_REMOVE_FLAT_NOISE
    uint8_t *sb_flat_noise_array;
#endif
    uint16_t non_moving_index_average; // used by ModeDecisionConfigurationProcess()
    int16_t  non_moving_index_min_distance;
    int16_t  non_moving_index_max_distance;
    uint16_t qp_scaling_average_complexity;
    // Histograms
    uint32_t ****picture_histogram;
    uint64_t     average_intensity_per_region[MAX_NUMBER_OF_REGIONS_IN_WIDTH]
                                         [MAX_NUMBER_OF_REGIONS_IN_HEIGHT][3];

    // Segments
    uint16_t me_segments_total_count;
    uint8_t  me_segments_column_count;
    uint8_t  me_segments_row_count;
    uint16_t me_segments_completion_count;
#if  FTR_TPL_TR
    uint64_t              me_trailing_segments_completion_count;
    EbHandle              pame_trail_done_semaphore;
#endif
#if  FTR_TPL_TR
    TPLData               tpl_data_trail;
    uint8_t               hierarchical_levels_trail;
    OisMbResults        **ois_mb_results_trail;
    MotionEstimationData *pa_me_data_trail;
    uint32_t             *rc_me_distortion_trail;
#if FTR_GM_OPT_BASED_ON_ME
#if TUNE_M9_GM_DETECTOR
    uint8_t *     stationary_block_present_sb_trail; // 1 when a % of the SB is stationary relative to reference frame(s) ((0,0) MV: decode order), 0 otherwise
#endif
    uint8_t *     rc_me_allow_gm_trail;
#endif
#if FTR_EARLY_DEPTH_REMOVAL
    uint32_t             *me_64x64_distortion_trail;
    uint32_t             *me_32x32_distortion_trail;
    uint32_t             *me_16x16_distortion_trail;
    uint32_t             *me_8x8_distortion_trail;
#endif
#if TUNE_DEPTH_REMOVAL_PER_RESOLUTION
    uint32_t *me_8x8_cost_variance_trail;
#endif
#endif
#if FTR_TPL_TR
    EbPictureBufferDesc       *non_tf_input;
#endif

    uint16_t inloop_me_segments_total_count;
    uint8_t  inloop_me_segments_column_count;
    uint8_t  inloop_me_segments_row_count;
    uint16_t inloop_me_segments_completion_count;

    // Motion Estimation Results
    uint8_t   max_number_of_pus_per_sb;
    uint32_t *rc_me_distortion;
#if FTR_GM_OPT_BASED_ON_ME
#if TUNE_M9_GM_DETECTOR
    uint8_t *     stationary_block_present_sb; // 1 when a % of the SB is stationary relative to reference frame(s) ((0,0) MV: decode order), 0 otherwise
#endif
    uint8_t *     rc_me_allow_gm;
#endif



#if TUNE_DEPTH_REMOVAL_PER_RESOLUTION
    uint32_t *me_8x8_cost_variance;
#endif
#if FTR_EARLY_DEPTH_REMOVAL
    uint32_t *me_64x64_distortion;
    uint32_t *me_32x32_distortion;
    uint32_t *me_16x16_distortion;
    uint32_t *me_8x8_distortion;
#endif
    // Global motion estimation results
    EbBool               is_global_motion[MAX_NUM_OF_REF_PIC_LIST][REF_LIST_MAX_DEPTH];
    EbWarpedMotionParams global_motion_estimation[MAX_NUM_OF_REF_PIC_LIST][REF_LIST_MAX_DEPTH];

#if !CLN_OLD_RC
    // Motion Estimation Distortion and OIS Historgram
    uint16_t *            me_distortion_histogram;
    uint16_t *            ois_distortion_histogram;
    uint32_t *            intra_sad_interval_index;
    uint32_t *            inter_sad_interval_index;
#endif
    uint16_t              me_processed_sb_count;
    EbHandle              me_processed_sb_mutex;
#if !CLN_OLD_RC
    EbHandle              rc_distortion_histogram_mutex;
#endif
    FirstPassData         firstpass_data;
    RefreshFrameFlagsInfo refresh_frame;
    int                   internal_altref_allowed;
    int64_t               ts_duration;
    OisMbResults **       ois_mb_results;
    TplStats **           tpl_stats;
    int32_t               is_720p_or_larger;
    int32_t               base_rdmult;
    double                r0;
    double *              tpl_beta;
    double *              tpl_rdmult_scaling_factors;
    double *              tpl_sb_rdmult_scaling_factors;
    EbBool                blk_lambda_tuning;
    // Dynamic GOP
    EbPred   pred_structure;
    uint8_t  hierarchical_levels;
#if !CLN_OLD_RC
    uint16_t full_sb_count;
#endif
    EbBool   init_pred_struct_position_flag;
    int8_t   hierarchical_layers_diff;
    //Dep-Cnt Clean up is done using 2 mechanism
    //1: a triggering picture that will clean up all previous pictures;
    //2: a picture does a self clean up
    int32_t
        self_updated_links; // if negative: number of pic not dependent on curr; usefull for pictures in current MG which have a dec order > Base-Intra
        //due to I frame Insertion
    DepCntPicInfo updated_links_arr
        [UPDATED_LINKS]; //if not empty, this picture is a depn-cnt-cleanUp triggering picture (I frame; or MG size change )
        //this array will store all others pictures needing a dep-cnt clean up.
    uint32_t
        other_updated_links_cnt; //how many other pictures in the above array needing a dep-cnt clean-up
    // HME Flags
    EbBool enable_hme_flag;
    EbBool enable_hme_level0_flag;
    EbBool enable_hme_level1_flag;
    EbBool enable_hme_level2_flag;

    // HME Flags form Temporal Filtering
    EbBool tf_enable_hme_flag;
    EbBool tf_enable_hme_level0_flag;
    EbBool tf_enable_hme_level1_flag;
    EbBool tf_enable_hme_level2_flag;

    // MD
    EbEncMode         enc_mode;
    EB_SB_DEPTH_MODE *sb_depth_mode_array;
    // Multi-modes signal(s)
    MultiPassPdLevel multi_pass_pd_level;
    EbBool           disallow_nsq;
    EbBool           disallow_all_nsq_blocks_below_8x8;
    EbBool           disallow_all_nsq_blocks_below_16x16;
    EbBool           disallow_all_non_hv_nsq_blocks_below_16x16;
    EbBool           disallow_all_h4_v4_blocks_below_16x16;

    EbBool  disallow_all_nsq_blocks_below_64x64; //disallow nsq in 64x64 and below
    EbBool  disallow_all_nsq_blocks_below_32x32; //disallow nsq in 32x32 and below
    EbBool  disallow_all_nsq_blocks_above_64x64; //disallow nsq in 64x64 and above
    EbBool  disallow_all_nsq_blocks_above_32x32; //disallow nsq in 32x32 and above
    EbBool  disallow_all_nsq_blocks_above_16x16; //disallow nsq in 16x16 and above
    EbBool  disallow_HV4; //disallow             H4/V4
    EbBool  disallow_HVA_HVB_HV4; //disallow HA/HB/VA/VB H4/V4
    uint8_t loop_filter_mode;
    uint8_t intra_pred_mode;
    uint8_t tx_size_search_mode;
    uint8_t frame_end_cdf_update_mode; // mm-signal: 0: OFF, 1:ON
    //**********************************************************************************************************//
    Av1RpsNode av1_ref_signal;
    EbBool     has_show_existing;
    int32_t    ref_frame_map[REF_FRAMES]; /* maps fb_idx to reference slot */
    int32_t    is_skip_mode_allowed;
    int32_t    skip_mode_flag;
#if SIM_OLD_REF
    uint32_t   pic_index;  //index of picture in the mg
#endif
    // Flag for a frame used as a reference - not written to the Bitstream
    int32_t is_reference_frame;

    // Flag signaling that the frame is encoded using only INTRA modes.
    uint8_t intra_only;
    /* profile settings */
#if CONFIG_ENTROPY_STATS
    int32_t coef_cdf_category;
#endif
    int32_t separate_uv_delta_q;

    // Global quant matrix tables
    const QmVal *giqmatrix[NUM_QM_LEVELS][3][TX_SIZES_ALL];
    const QmVal *gqmatrix[NUM_QM_LEVELS][3][TX_SIZES_ALL];
    int32_t      min_qmlevel;
    int32_t      max_qmlevel;
    // Encoder
    LoopFilterInfoN lf_info;

    // Flag signaling how frame contexts should be updated at the end of
    // a frame decode
    RefreshFrameContextMode refresh_frame_context;
    uint32_t                frame_context_idx; /* Context to use/update */
    int32_t                 fb_of_context_type[REF_FRAMES];
    uint64_t                frame_offset;
    uint32_t                large_scale_tile;
    int32_t                 nb_cdef_strengths;
    // Resolution of delta quant
    int32_t monochrome;
    int32_t prev_qindex[MAX_TILE_CNTS];

    // Since actual frame level loop filtering level value is not available
    // at the beginning of the tile (only available during actual filtering)
    // at encoder side.we record the delta_lf (against the frame level loop
    // filtering level) and code the delta between previous superblock's delta
    // lf and current delta lf. It is equivalent to the delta between previous
    // superblock's actual lf and current lf.
    int32_t prev_delta_lf_from_base;
    // For this experiment, we have four frame filter levels for different plane
    // and direction. So, to support the per superblock update, we need to add
    // a few more params as below.
    // 0: delta loop filter level for y plane vertical
    // 1: delta loop filter level for y plane horizontal
    // 2: delta loop filter level for u plane
    // 3: delta loop filter level for v plane
    // To make it consistent with the reference to each filter level in segment,
    // we need to -1, since
    // SEG_LVL_ALT_LF_Y_V = 1;
    // SEG_LVL_ALT_LF_Y_H = 2;
    // SEG_LVL_ALT_LF_U   = 3;
    // SEG_LVL_ALT_LF_V   = 4;
    //
    int32_t prev_delta_lf[FRAME_LF_COUNT];
    int32_t curr_delta_lf[FRAME_LF_COUNT];

    // Resolution of delta quant
    // int32_t delta_q_res;
    int32_t              allow_comp_inter_inter;
    int16_t              pan_mvx;
    int16_t              pan_mvy;
    int16_t              tilt_mvx;
    int16_t              tilt_mvy;
    EbWarpedMotionParams global_motion[TOTAL_REFS_PER_FRAME];
    PictureControlSet *  child_pcs;
#if CLN_STRUCT
    EncDecSet *  enc_dec_ptr;
#endif
    Macroblock *         av1x;
    int32_t film_grain_params_present; //todo (AN): Do we need this flag at picture level?
    AomDenoiseAndModel *denoise_and_model;
#if !CLN_RES_PROCESS
    RestUnitSearchInfo *rusi_picture[3]; //for 3 planes
#endif
    int8_t              cdef_level;
    uint8_t             palette_level;
#if FTR_ALIGN_SC_DETECOR
    uint8_t             sc_class0;
    uint8_t             sc_class1;
    uint8_t             sc_class2;
#else
    uint8_t             sc_content_detected;
#endif
    uint8_t             ibc_mode;
    SkipModeInfo        skip_mode_info;
    uint64_t picture_number_alt; // The picture number overlay includes all the overlay frames
    uint8_t  is_alt_ref;
    uint8_t  is_overlay;
    struct PictureParentControlSet *overlay_ppcs_ptr;
    struct PictureParentControlSet *alt_ref_ppcs_ptr;
#if !TUNE_REDESIGN_TF_CTRLS
    uint8_t                         altref_strength;
#endif
    double                          noise_levels[MAX_MB_PLANE];
    int32_t                         pic_decision_reorder_queue_idx;
    struct PictureParentControlSet *temp_filt_pcs_list[ALTREF_MAX_NFRAMES];
    EbByte                          save_enhanced_picture_ptr[3];
    EbByte                          save_enhanced_picture_bit_inc_ptr[3];
    EbHandle                        temp_filt_done_semaphore;
    EbHandle                        temp_filt_mutex;
    EbHandle                        debug_mutex;

    uint8_t  temp_filt_prep_done;
    uint16_t temp_filt_seg_acc;
#if TPL_KERNEL
    EbHandle tpl_disp_done_semaphore;
    EbHandle tpl_disp_done_semaphore_trail;
#endif
    // TPL ME
    EbHandle     tpl_me_done_semaphore;
    EbHandle     tpl_me_mutex;
    uint16_t     tpl_me_seg_acc;
    int16_t      tpl_me_segments_total_count;
    uint8_t      tpl_me_segments_column_count;
    uint8_t      tpl_me_segments_row_count;
    uint8_t      tpl_me_done;
    AtomicVarU32 pame_done; //set when PA ME is done.
#if FIX_DDL
    CondVar me_ready;
#else
    EbHandle pame_done_semaphore;
#endif
#if !FTR_USE_LAD_TPL
    uint8_t      num_tpl_grps;
    uint8_t      num_tpl_processed;
#endif
    int16_t      tf_segments_total_count;
    uint8_t      tf_segments_column_count;
    uint8_t      tf_segments_row_count;
    uint8_t      past_altref_nframes;
    uint8_t      future_altref_nframes;
    EbBool       temporal_filtering_on;
#if !TUNE_REDESIGN_TF_CTRLS
    uint64_t
        filtered_sse; // the normalized SSE between filtered and original alt_ref with 8 bit precision.
    // I Slice has the value of the next ALT_REF picture
#endif
    uint64_t    filtered_sse_uv;
    FrameHeader frm_hdr;
    uint16_t *  altref_buffer_highbd[3];
    uint8_t     pic_obmc_level;
    uint8_t     gm_level;
    uint8_t     tx_size_early_exit;

    SbParams *        sb_params_array;
    SbGeom *          sb_geom;
    EbInputResolution input_resolution;
    uint16_t          picture_sb_width;
    uint16_t          picture_sb_height;
    uint16_t          sb_total_count_unscaled;

    // Picture dimensions (resized or not)
    // aligned to be a multiple of 8 pixels
    uint16_t aligned_width;
    uint16_t aligned_height;

    // Picture dimensions (resized or not)
    // --not-- aligned to be a multiple of 8 pixels
    uint16_t frame_width;
    uint16_t frame_height;

    EbBool                frame_superres_enabled;
    uint8_t               superres_denom;
    EbObjectWrapper *     me_data_wrapper_ptr;
    MotionEstimationData *pa_me_data;
    unsigned char         gf_group_index;
    struct PictureParentControlSet
        *    tpl_group[MAX_TPL_GROUP_SIZE]; //stores pcs pictures needed for tpl algorithm
    uint32_t tpl_group_size; //size of above buffer
    void *   pd_window
        [PD_WINDOW_SIZE]; //stores previous, current, future pictures from pd-reord-queue. empty for first I.
    uint8_t pd_window_count;

#if FTR_LAD_MG
    struct PictureParentControlSet
        *ext_group[MAX_TPL_EXT_GROUP_SIZE]; //stores pcs pictures needed for lad mg based algorithms
    uint32_t ext_group_size; //actual size of extended group

    int64_t  ext_mg_id;
    int64_t  ext_mg_size; //same as mg expect for MGops with [LDP-I] which are split into 2

    struct PictureParentControlSet *ntpl_group[MAX_TPL_EXT_GROUP_SIZE]; //new tpl group formed the extended group
#if FTR_USE_LAD_TPL
    uint8_t tpl_valid_pic[MAX_TPL_EXT_GROUP_SIZE];
    uint8_t used_tpl_frame_num;
#endif
    uint32_t ntpl_group_size;
#endif

    // For TPL, in addition to frames in the minigop size, we might process extra frames from the next minigop. These frames are
    // called trailing frames. Trailing frames are available because of TF and their minigop is not determined yet. As a result,
    // when used in RC there is a risk of race condition to access the PCS data. To prevent the problem, TplData should be used instead of PCS.
    uint8_t tpl_trailing_frame_count;
    // Tune TPL for better chroma.Only for 240P
    uint8_t    tune_tpl_for_chroma;
    uint8_t    is_next_frame_intra;
#if FTR_SCALE_FACTOR
    uint8_t is_superres_none;
#endif
    TfControls tf_ctrls;
    GmControls gm_ctrls;
#if FTR_VBR_MT
    // RC related variables
#else
    // Loop variables
#endif
    int                             q_low;
    int                             q_high;
    int                             loop_count;
    int                             overshoot_seen;
    int                             undershoot_seen;
    int                             low_cr_seen;
    uint64_t                        pcs_total_rate;
#if FIX_VBR_MISMACTH
    EbHandle                        pcs_total_rate_mutex;
#endif
    int16_t                         first_pass_seg_total_count;
    uint8_t                         first_pass_seg_column_count;
    uint8_t                         first_pass_seg_row_count;
    uint16_t                        first_pass_seg_acc;
    EbHandle                        first_pass_done_semaphore;
    EbHandle                        first_pass_mutex;
    struct PictureParentControlSet *first_pass_ref_ppcs_ptr[2];
    uint8_t                         first_pass_ref_count;
    uint8_t                         first_pass_done;
#if FTR_TPL_TR
    TplControls                     tpl_ctrls;
#endif
#if FIX_ADD_TPL_VALID
    uint8_t tpl_is_valid;
#endif
#if FTR_SIMULATE_P_BASE
    uint8_t                         list0_only_base; // Use list0 only if BASE (mimik a P)
#endif
#if TPL_SEG


    EbHandle tpl_disp_mutex;
    EbHandle tpl_disp_mutex_trail;
    //uint32_t         input_type;
    int16_t          enc_dec_segment_row;
    uint16_t         tile_group_index;
    uint16_t         tpl_disp_coded_sb_count;
    uint16_t         tpl_disp_coded_sb_count_trail;
#endif
#if TPL_SEG

    uint16_t sb_total_count_pix;
    EncDecSegments **tpl_disp_segment_ctrl;
    EncDecSegments **tpl_disp_segment_ctrl_trail;
#endif
#if FTR_VBR_MT
    // the offsets for STATS_BUFFER_CTX
    uint64_t                        stats_in_end_offset;
    // the offsets for stats_in
    uint64_t                        stats_in_offset;
#endif
#if FTR_VBR_MT
    //GF_GROUP parameters store in PCS
    int                             update_type;
    int                             layer_depth;
    int                             arf_boost;
    int                             gf_group_size;
    //RATE_CONTROL parameters store in PCS
    int                             base_frame_target; // A baseline frame target before adjustment.
    int                             this_frame_target; // Actual frame target after rc adjustment.
    int                             projected_frame_size;

    int                             frames_to_key;
    int                             frames_since_key;
    int                             is_src_frame_alt_ref;
    // Total number of stats used only for gfu_boost calculation.
    int                             num_stats_used_for_gfu_boost;
    // Total number of stats required by gfu_boost calculation.
    int                             num_stats_required_for_gfu_boost;
    int                             top_index;
    int                             bottom_index;
    // stores gf group (minigop) length
    int                             gf_interval;
    int                             gf_update_due; // thr gf update in RC is due for I, or base frames (except the one after I) or P frames
    uint8_t                         is_new_gf_group;
    struct PictureParentControlSet *gf_group[MAX_TPL_GROUP_SIZE];
#endif
#if FTR_REDUCE_MVEST
    uint8_t bypass_cost_table_gen;
#endif
} PictureParentControlSet;

typedef struct PictureControlSetInitData {
    uint16_t       picture_width;
    uint16_t       picture_height;
    uint16_t       left_padding;
    uint16_t       right_padding;
    uint16_t       top_padding;
    uint16_t       bot_padding;
    EbBitDepthEnum bit_depth;
    EbColorFormat  color_format;
    uint32_t       sb_sz;
    uint8_t        cfg_palette;
    uint32_t
        sb_size_pix; //since we still have lot of code assuming 64x64 SB, we add a new paramter supporting both128x128 and 64x64,
    //ultimately the fixed code supporting 64x64 should be upgraded to use 128x128 and the above could be removed.
    uint32_t max_depth;
    //EbBool                             is_16bit;
    uint32_t  ten_bit_format;
    uint32_t  compressed_ten_bit_format;
    uint16_t  enc_dec_segment_col;
    uint16_t  enc_dec_segment_row;
    EbEncMode enc_mode;
#if CLN_REST
    EbSvtAv1EncConfiguration static_config;
#endif
    uint8_t   speed_control;
    int8_t    hbd_mode_decision;
    uint16_t  film_grain_noise_level;
    EbBool    ext_block_flag;
    uint8_t   cdf_mode;
    uint8_t   over_boundary_block_mode;
    uint8_t   mfmv;
    //init value for child pcs
    uint8_t tile_row_count;
    uint8_t tile_column_count;

    //Init value for parent pcs
    uint8_t log2_tile_rows; //from command line
    uint8_t log2_tile_cols;
    uint8_t log2_sb_sz; //in mi unit
    EbBool  is_16bit_pipeline;

    uint16_t non_m8_pad_w;
    uint16_t non_m8_pad_h;
    uint8_t  enable_tpl_la;
    uint8_t  in_loop_ois;
#if CLN_RES_PROCESS && !CLN_BN
    RestorationInfo rst_info[MAX_MB_PLANE];
#endif

#if CLN_PPCS
    uint8_t  rc_firstpass_stats_out;
    uint32_t rate_control_mode;
#endif
#if CLN_BN

    Av1Common *                av1_cm;
#endif

#if OPT_ME
   uint8_t mrp_level;
#endif
} PictureControlSetInitData;

typedef struct Av1Comp {
    Yv12BufferConfig trial_frame_rst;
} Av1Comp;

/**************************************
     * Extern Function Declarations
     **************************************/
extern EbErrorType picture_control_set_creator(EbPtr *object_dbl_ptr, EbPtr object_init_data_ptr);
#if CLN_STRUCT
extern EbErrorType recon_coef_creator(EbPtr *object_dbl_ptr, EbPtr object_init_data_ptr);
#endif
extern EbErrorType picture_parent_control_set_creator(EbPtr *object_dbl_ptr,
                                                      EbPtr  object_init_data_ptr);
extern EbErrorType me_creator(EbPtr *object_dbl_ptr, EbPtr object_init_data_ptr);
#if  OPT_ME
extern EbErrorType me_sb_results_ctor(MeSbResults *obj_ptr,PictureControlSetInitData *init_data_ptr);
#else
extern EbErrorType me_sb_results_ctor(MeSbResults *obj_ptr);
#endif
#ifdef __cplusplus
}
#endif
#endif // EbPictureControlSet_h
