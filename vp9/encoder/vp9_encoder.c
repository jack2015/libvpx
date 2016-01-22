/*
 * Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <math.h>
#include <stdio.h>
#include <limits.h>

#include "./vpx_config.h"
#include "./vpx_scale_rtcd.h"
#include "vpx/internal/vpx_psnr.h"
#include "vpx_ports/vpx_timer.h"

#include "vp9/common/vp9_alloccommon.h"
#include "vp9/common/vp9_filter.h"
#include "vp9/common/vp9_idct.h"
#if CONFIG_VP9_POSTPROC
#include "vp9/common/vp9_postproc.h"
#endif
#include "vp9/common/vp9_reconinter.h"
#include "vp9/common/vp9_reconintra.h"
#include "vp9/common/vp9_systemdependent.h"
#include "vp9/common/vp9_tile_common.h"

#include "vp9/encoder/vp9_aq_complexity.h"
#include "vp9/encoder/vp9_aq_cyclicrefresh.h"
#include "vp9/encoder/vp9_aq_variance.h"
#include "vp9/encoder/vp9_bitstream.h"
#include "vp9/encoder/vp9_context_tree.h"
#include "vp9/encoder/vp9_encodeframe.h"
#include "vp9/encoder/vp9_encodemv.h"
#include "vp9/encoder/vp9_firstpass.h"
#include "vp9/encoder/vp9_mbgraph.h"
#include "vp9/encoder/vp9_encoder.h"
#include "vp9/encoder/vp9_picklpf.h"
#include "vp9/encoder/vp9_ratectrl.h"
#include "vp9/encoder/vp9_rd.h"
#include "vp9/encoder/vp9_segmentation.h"
#include "vp9/encoder/vp9_speed_features.h"
#if CONFIG_INTERNAL_STATS
#include "vp9/encoder/vp9_ssim.h"
#endif
#include "vp9/encoder/vp9_temporal_filter.h"
#include "vp9/encoder/vp9_resize.h"

void vp9_coef_tree_initialize();

#define SHARP_FILTER_QTHRESH 0          /* Q threshold for 8-tap sharp filter */

#define ALTREF_HIGH_PRECISION_MV 1      // Whether to use high precision mv
                                         //  for altref computation.
#define HIGH_PRECISION_MV_QTHRESH 200   // Q threshold for high precision
                                         // mv. Choose a very high value for
                                         // now so that HIGH_PRECISION is always
                                         // chosen.

// #define OUTPUT_YUV_REC

#ifdef OUTPUT_YUV_DENOISED
FILE *yuv_denoised_file = NULL;
#endif
#ifdef OUTPUT_YUV_REC
FILE *yuv_rec_file;
#endif

#if 0
FILE *framepsnr;
FILE *kf_list;
FILE *keyfile;
#endif

static INLINE void Scale2Ratio(VPX_SCALING mode, int *hr, int *hs) {
  switch (mode) {
    case NORMAL:
      *hr = 1;
      *hs = 1;
      break;
    case FOURFIVE:
      *hr = 4;
      *hs = 5;
      break;
    case THREEFIVE:
      *hr = 3;
      *hs = 5;
    break;
    case ONETWO:
      *hr = 1;
      *hs = 2;
    break;
    default:
      *hr = 1;
      *hs = 1;
       assert(0);
      break;
  }
}

void vp9_set_high_precision_mv(VP9_COMP *cpi, int allow_high_precision_mv) {
  MACROBLOCK *const mb = &cpi->mb;
  cpi->common.allow_high_precision_mv = allow_high_precision_mv;
  if (cpi->common.allow_high_precision_mv) {
    mb->mvcost = mb->nmvcost_hp;
    mb->mvsadcost = mb->nmvsadcost_hp;
  } else {
    mb->mvcost = mb->nmvcost;
    mb->mvsadcost = mb->nmvsadcost;
  }
}

static void setup_frame(VP9_COMP *cpi) {
  VP9_COMMON *const cm = &cpi->common;
  // Set up entropy context depending on frame type. The decoder mandates
  // the use of the default context, index 0, for keyframes and inter
  // frames where the error_resilient_mode or intra_only flag is set. For
  // other inter-frames the encoder currently uses only two contexts;
  // context 1 for ALTREF frames and context 0 for the others.
  if (frame_is_intra_only(cm) || cm->error_resilient_mode) {
    vp9_setup_past_independence(cm);
  } else {
    cm->frame_context_idx = cpi->refresh_alt_ref_frame;
  }

  if (cm->frame_type == KEY_FRAME) {
    cpi->refresh_golden_frame = 1;
    cpi->refresh_alt_ref_frame = 1;
    vp9_zero(cpi->interp_filter_selected);
  } else {
    cm->fc = cm->frame_contexts[cm->frame_context_idx];
    vp9_zero(cpi->interp_filter_selected[0]);
  }
}

void vp9_initialize_enc() {
  static int init_done = 0;

  if (!init_done) {
    vp9_rtcd();
    vp9_init_intra_predictors();
    vp9_coef_tree_initialize();
    vp9_tokenize_initialize();
    vp9_init_me_luts();
    vp9_rc_init_minq_luts();
    vp9_entropy_mv_init();
    vp9_entropy_mode_init();
    vp9_temporal_filter_init();
#if CONFIG_WEDGE_PARTITION
    vp9_init_wedge_masks();
#endif
    init_done = 1;
  }
}

static void dealloc_compressor_data(VP9_COMP *cpi) {
  VP9_COMMON *const cm = &cpi->common;

  // Delete sementation map
  vpx_free(cpi->segmentation_map);
  cpi->segmentation_map = NULL;
  vpx_free(cm->last_frame_seg_map);
  cm->last_frame_seg_map = NULL;
  vpx_free(cpi->coding_context.last_frame_seg_map_copy);
  cpi->coding_context.last_frame_seg_map_copy = NULL;

  vpx_free(cpi->complexity_map);
  cpi->complexity_map = NULL;

#if CONFIG_INTRABC
  vpx_free(cpi->ndvcosts[0]);
  vpx_free(cpi->ndvcosts[1]);
  cpi->ndvcosts[0] = NULL;
  cpi->ndvcosts[1] = NULL;
#endif  // CONFIG_INTRABC

  vpx_free(cpi->nmvcosts[0]);
  vpx_free(cpi->nmvcosts[1]);
  cpi->nmvcosts[0] = NULL;
  cpi->nmvcosts[1] = NULL;

  vpx_free(cpi->nmvcosts_hp[0]);
  vpx_free(cpi->nmvcosts_hp[1]);
  cpi->nmvcosts_hp[0] = NULL;
  cpi->nmvcosts_hp[1] = NULL;

  vpx_free(cpi->nmvsadcosts[0]);
  vpx_free(cpi->nmvsadcosts[1]);
  cpi->nmvsadcosts[0] = NULL;
  cpi->nmvsadcosts[1] = NULL;

  vpx_free(cpi->nmvsadcosts_hp[0]);
  vpx_free(cpi->nmvsadcosts_hp[1]);
  cpi->nmvsadcosts_hp[0] = NULL;
  cpi->nmvsadcosts_hp[1] = NULL;

  vp9_cyclic_refresh_free(cpi->cyclic_refresh);
  cpi->cyclic_refresh = NULL;

  vp9_free_ref_frame_buffers(cm);
  vp9_free_context_buffers(cm);

  vp9_free_frame_buffer(&cpi->last_frame_uf);
#if CONFIG_LOOP_POSTFILTER
  vp9_free_frame_buffer(&cpi->last_frame_db);
#endif  // CONFIG_LOOP_POSTFILTER
  vp9_free_frame_buffer(&cpi->scaled_source);
  vp9_free_frame_buffer(&cpi->scaled_last_source);
  vp9_free_frame_buffer(&cpi->alt_ref_buffer);
  vp9_lookahead_destroy(cpi->lookahead);

  vpx_free(cpi->tok);
  cpi->tok = 0;

  vp9_free_pc_tree(cpi);

  if (cpi->source_diff_var != NULL) {
    vpx_free(cpi->source_diff_var);
    cpi->source_diff_var = NULL;
  }
}

static void save_coding_context(VP9_COMP *cpi) {
  CODING_CONTEXT *const cc = &cpi->coding_context;
  VP9_COMMON *cm = &cpi->common;

  // Stores a snapshot of key state variables which can subsequently be
  // restored with a call to vp9_restore_coding_context. These functions are
  // intended for use in a re-code loop in vp9_compress_frame where the
  // quantizer value is adjusted between loop iterations.
  vp9_copy(cc->nmvjointcost,  cpi->mb.nmvjointcost);

#if CONFIG_INTRABC
  vpx_memcpy(cc->ndvcosts[0], cpi->ndvcosts[0],
             MV_VALS * sizeof(*cpi->ndvcosts[0]));
  vpx_memcpy(cc->ndvcosts[1], cpi->ndvcosts[1],
             MV_VALS * sizeof(*cpi->ndvcosts[1]));
#endif  // CONFIG_INTRABC
  vpx_memcpy(cc->nmvcosts[0], cpi->nmvcosts[0],
             MV_VALS * sizeof(*cpi->nmvcosts[0]));
  vpx_memcpy(cc->nmvcosts[1], cpi->nmvcosts[1],
             MV_VALS * sizeof(*cpi->nmvcosts[1]));
  vpx_memcpy(cc->nmvcosts_hp[0], cpi->nmvcosts_hp[0],
             MV_VALS * sizeof(*cpi->nmvcosts_hp[0]));
  vpx_memcpy(cc->nmvcosts_hp[1], cpi->nmvcosts_hp[1],
             MV_VALS * sizeof(*cpi->nmvcosts_hp[1]));

  vp9_copy(cc->segment_pred_probs, cm->seg.pred_probs);

  vpx_memcpy(cpi->coding_context.last_frame_seg_map_copy,
             cm->last_frame_seg_map, (cm->mi_rows * cm->mi_cols));

  vp9_copy(cc->last_ref_lf_deltas, cm->lf.last_ref_deltas);
  vp9_copy(cc->last_mode_lf_deltas, cm->lf.last_mode_deltas);

  cc->fc = cm->fc;
}

static void restore_coding_context(VP9_COMP *cpi) {
  CODING_CONTEXT *const cc = &cpi->coding_context;
  VP9_COMMON *cm = &cpi->common;

  // Restore key state variables to the snapshot state stored in the
  // previous call to vp9_save_coding_context.
  vp9_copy(cpi->mb.nmvjointcost, cc->nmvjointcost);

#if CONFIG_INTRABC
  vpx_memcpy(cpi->ndvcosts[0], cc->ndvcosts[0],
             MV_VALS * sizeof(*cc->ndvcosts[0]));
  vpx_memcpy(cpi->ndvcosts[1], cc->ndvcosts[1],
             MV_VALS * sizeof(*cc->ndvcosts[1]));
#endif  // CONFIG_INTRABC
  vpx_memcpy(cpi->nmvcosts[0], cc->nmvcosts[0],
             MV_VALS * sizeof(*cc->nmvcosts[0]));
  vpx_memcpy(cpi->nmvcosts[1], cc->nmvcosts[1],
             MV_VALS * sizeof(*cc->nmvcosts[1]));
  vpx_memcpy(cpi->nmvcosts_hp[0], cc->nmvcosts_hp[0],
             MV_VALS * sizeof(*cc->nmvcosts_hp[0]));
  vpx_memcpy(cpi->nmvcosts_hp[1], cc->nmvcosts_hp[1],
             MV_VALS * sizeof(*cc->nmvcosts_hp[1]));

  vp9_copy(cm->seg.pred_probs, cc->segment_pred_probs);

  vpx_memcpy(cm->last_frame_seg_map,
             cpi->coding_context.last_frame_seg_map_copy,
             (cm->mi_rows * cm->mi_cols));

  vp9_copy(cm->lf.last_ref_deltas, cc->last_ref_lf_deltas);
  vp9_copy(cm->lf.last_mode_deltas, cc->last_mode_lf_deltas);

  cm->fc = cc->fc;
}

static void configure_static_seg_features(VP9_COMP *cpi) {
  VP9_COMMON *const cm = &cpi->common;
  const RATE_CONTROL *const rc = &cpi->rc;
  struct segmentation *const seg = &cm->seg;

  int high_q = (int)(rc->avg_q > 48.0);
  int qi_delta;

  // Disable and clear down for KF
  if (cm->frame_type == KEY_FRAME) {
    // Clear down the global segmentation map
    vpx_memset(cpi->segmentation_map, 0, cm->mi_rows * cm->mi_cols);
    seg->update_map = 0;
    seg->update_data = 0;
    cpi->static_mb_pct = 0;

    // Disable segmentation
    vp9_disable_segmentation(seg);

    // Clear down the segment features.
    vp9_clearall_segfeatures(seg);
  } else if (cpi->refresh_alt_ref_frame) {
    // If this is an alt ref frame
    // Clear down the global segmentation map
    vpx_memset(cpi->segmentation_map, 0, cm->mi_rows * cm->mi_cols);
    seg->update_map = 0;
    seg->update_data = 0;
    cpi->static_mb_pct = 0;

    // Disable segmentation and individual segment features by default
    vp9_disable_segmentation(seg);
    vp9_clearall_segfeatures(seg);

    // Scan frames from current to arf frame.
    // This function re-enables segmentation if appropriate.
    vp9_update_mbgraph_stats(cpi);

    // If segmentation was enabled set those features needed for the
    // arf itself.
    if (seg->enabled) {
      seg->update_map = 1;
      seg->update_data = 1;

      qi_delta = vp9_compute_qdelta(rc, rc->avg_q, rc->avg_q * 0.875,
                                    cm->bit_depth);
      vp9_set_segdata(seg, 1, SEG_LVL_ALT_Q, qi_delta - 2);
      vp9_set_segdata(seg, 1, SEG_LVL_ALT_LF, -2);

      vp9_enable_segfeature(seg, 1, SEG_LVL_ALT_Q);
      vp9_enable_segfeature(seg, 1, SEG_LVL_ALT_LF);

      // Where relevant assume segment data is delta data
      seg->abs_delta = SEGMENT_DELTADATA;
    }
  } else if (seg->enabled) {
    // All other frames if segmentation has been enabled

    // First normal frame in a valid gf or alt ref group
    if (rc->frames_since_golden == 0) {
      // Set up segment features for normal frames in an arf group
      if (rc->source_alt_ref_active) {
        seg->update_map = 0;
        seg->update_data = 1;
        seg->abs_delta = SEGMENT_DELTADATA;

        qi_delta = vp9_compute_qdelta(rc, rc->avg_q, rc->avg_q * 1.125,
                                      cm->bit_depth);
        vp9_set_segdata(seg, 1, SEG_LVL_ALT_Q, qi_delta + 2);
        vp9_enable_segfeature(seg, 1, SEG_LVL_ALT_Q);

        vp9_set_segdata(seg, 1, SEG_LVL_ALT_LF, -2);
        vp9_enable_segfeature(seg, 1, SEG_LVL_ALT_LF);

        // Segment coding disabled for compred testing
        if (high_q || (cpi->static_mb_pct == 100)) {
          vp9_set_segdata(seg, 1, SEG_LVL_REF_FRAME, ALTREF_FRAME);
          vp9_enable_segfeature(seg, 1, SEG_LVL_REF_FRAME);
          vp9_enable_segfeature(seg, 1, SEG_LVL_SKIP);
        }
      } else {
        // Disable segmentation and clear down features if alt ref
        // is not active for this group

        vp9_disable_segmentation(seg);

        vpx_memset(cpi->segmentation_map, 0, cm->mi_rows * cm->mi_cols);

        seg->update_map = 0;
        seg->update_data = 0;

        vp9_clearall_segfeatures(seg);
      }
    } else if (rc->is_src_frame_alt_ref) {
      // Special case where we are coding over the top of a previous
      // alt ref frame.
      // Segment coding disabled for compred testing

      // Enable ref frame features for segment 0 as well
      vp9_enable_segfeature(seg, 0, SEG_LVL_REF_FRAME);
      vp9_enable_segfeature(seg, 1, SEG_LVL_REF_FRAME);

      // All mbs should use ALTREF_FRAME
      vp9_clear_segdata(seg, 0, SEG_LVL_REF_FRAME);
      vp9_set_segdata(seg, 0, SEG_LVL_REF_FRAME, ALTREF_FRAME);
      vp9_clear_segdata(seg, 1, SEG_LVL_REF_FRAME);
      vp9_set_segdata(seg, 1, SEG_LVL_REF_FRAME, ALTREF_FRAME);

      // Skip all MBs if high Q (0,0 mv and skip coeffs)
      if (high_q) {
        vp9_enable_segfeature(seg, 0, SEG_LVL_SKIP);
        vp9_enable_segfeature(seg, 1, SEG_LVL_SKIP);
      }
      // Enable data update
      seg->update_data = 1;
    } else {
      // All other frames.

      // No updates.. leave things as they are.
      seg->update_map = 0;
      seg->update_data = 0;
    }
  }
}

static void update_reference_segmentation_map(VP9_COMP *cpi) {
  VP9_COMMON *const cm = &cpi->common;
  MODE_INFO *mi_8x8_ptr = cm->mi;
  uint8_t *cache_ptr = cm->last_frame_seg_map;
  int row, col;

  for (row = 0; row < cm->mi_rows; row++) {
    MODE_INFO *mi_8x8 = mi_8x8_ptr;
    uint8_t *cache = cache_ptr;
    for (col = 0; col < cm->mi_cols; col++, mi_8x8++, cache++)
      cache[0] = mi_8x8[0].src_mi->mbmi.segment_id;
    mi_8x8_ptr += cm->mi_stride;
    cache_ptr += cm->mi_cols;
  }
}

static void alloc_raw_frame_buffers(VP9_COMP *cpi) {
  VP9_COMMON *cm = &cpi->common;
  const VP9EncoderConfig *oxcf = &cpi->oxcf;

  cpi->lookahead = vp9_lookahead_init(oxcf->width, oxcf->height,
                                      cm->subsampling_x, cm->subsampling_y,
#if CONFIG_VP9_HIGHBITDEPTH
                                      cm->use_highbitdepth,
#endif
                                      oxcf->lag_in_frames);
  if (!cpi->lookahead)
    vpx_internal_error(&cm->error, VPX_CODEC_MEM_ERROR,
                       "Failed to allocate lag buffers");

  if (vp9_realloc_frame_buffer(&cpi->alt_ref_buffer,
                               oxcf->width, oxcf->height,
                               cm->subsampling_x, cm->subsampling_y,
#if CONFIG_VP9_HIGHBITDEPTH
                               cm->use_highbitdepth,
#endif
                               VP9_ENC_BORDER_IN_PIXELS, NULL, NULL, NULL))
    vpx_internal_error(&cm->error, VPX_CODEC_MEM_ERROR,
                       "Failed to allocate altref buffer");
}

static void alloc_ref_frame_buffers(VP9_COMP *cpi) {
  VP9_COMMON *const cm = &cpi->common;
  if (vp9_alloc_ref_frame_buffers(cm, cm->width, cm->height))
    vpx_internal_error(&cm->error, VPX_CODEC_MEM_ERROR,
                       "Failed to allocate frame buffers");
}

static void alloc_util_frame_buffers(VP9_COMP *cpi) {
  VP9_COMMON *const cm = &cpi->common;
  if (vp9_realloc_frame_buffer(&cpi->last_frame_uf,
                               cm->width, cm->height,
                               cm->subsampling_x, cm->subsampling_y,
#if CONFIG_VP9_HIGHBITDEPTH
                               cm->use_highbitdepth,
#endif
                               VP9_ENC_BORDER_IN_PIXELS, NULL, NULL, NULL))
    vpx_internal_error(&cm->error, VPX_CODEC_MEM_ERROR,
                       "Failed to allocate last frame buffer");

#if CONFIG_LOOP_POSTFILTER
  if (vp9_realloc_frame_buffer(&cpi->last_frame_db,
                               cm->width, cm->height,
                               cm->subsampling_x, cm->subsampling_y,
#if CONFIG_VP9_HIGHBITDEPTH
                               cm->use_highbitdepth,
#endif
                               VP9_ENC_BORDER_IN_PIXELS, NULL, NULL, NULL))
    vpx_internal_error(&cm->error, VPX_CODEC_MEM_ERROR,
                       "Failed to allocate last frame deblocked buffer");
#endif  // CONFIG_LOOP_POSTFILTER

  if (vp9_realloc_frame_buffer(&cpi->scaled_source,
                               cm->width, cm->height,
                               cm->subsampling_x, cm->subsampling_y,
#if CONFIG_VP9_HIGHBITDEPTH
                               cm->use_highbitdepth,
#endif
                               VP9_ENC_BORDER_IN_PIXELS, NULL, NULL, NULL))
    vpx_internal_error(&cm->error, VPX_CODEC_MEM_ERROR,
                       "Failed to allocate scaled source buffer");

  if (vp9_realloc_frame_buffer(&cpi->scaled_last_source,
                               cm->width, cm->height,
                               cm->subsampling_x, cm->subsampling_y,
#if CONFIG_VP9_HIGHBITDEPTH
                               cm->use_highbitdepth,
#endif
                               VP9_ENC_BORDER_IN_PIXELS, NULL, NULL, NULL))
    vpx_internal_error(&cm->error, VPX_CODEC_MEM_ERROR,
                       "Failed to allocate scaled last source buffer");
}

void vp9_alloc_compressor_data(VP9_COMP *cpi) {
  VP9_COMMON *cm = &cpi->common;

  vp9_alloc_context_buffers(cm, cm->width, cm->height);

  vpx_free(cpi->tok);

  {
    unsigned int tokens = get_token_alloc(cm->mb_rows, cm->mb_cols);
    CHECK_MEM_ERROR(cm, cpi->tok, vpx_calloc(tokens, sizeof(*cpi->tok)));
  }
  vp9_setup_pc_tree(&cpi->common, cpi);
}

static void update_frame_size(VP9_COMP *cpi) {
  VP9_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &cpi->mb.e_mbd;

  vp9_set_mb_mi(cm, cm->width, cm->height);
  vp9_init_context_buffers(cm);
  init_macroblockd(cm, xd);
}

void vp9_new_framerate(VP9_COMP *cpi, double framerate) {
  cpi->framerate = framerate < 0.1 ? 30 : framerate;
  vp9_rc_update_framerate(cpi);
}

static void set_tile_limits(VP9_COMP *cpi) {
  VP9_COMMON *const cm = &cpi->common;

#if CONFIG_ROW_TILE
  cm->tile_width  = clamp(cpi->oxcf.tile_columns + 1,
                          1, 64) << MI_BLOCK_SIZE_LOG2;
  cm->tile_height = clamp(cpi->oxcf.tile_rows + 1,
                          1, 64) << MI_BLOCK_SIZE_LOG2;

  cm->tile_width  = MIN(cm->tile_width, cm->mi_cols);
  cm->tile_height = MIN(cm->tile_height, cm->mi_rows);

  // Get tile numbers
  cm->tile_cols = 1;
  while (cm->tile_cols * cm->tile_width < cm->mi_cols)
    ++cm->tile_cols;

  cm->tile_rows = 1;
  while (cm->tile_rows * cm->tile_height < cm->mi_rows)
    ++cm->tile_rows;

#else
  int min_log2_tiles, max_log2_tiles;
  vp9_get_tile_n_bits(cm->mi_cols, &min_log2_tiles, &max_log2_tiles);

  cm->log2_tile_cols = clamp(cpi->oxcf.tile_columns,
                             min_log2_tiles, max_log2_tiles);

  cm->log2_tile_rows = cpi->oxcf.tile_rows;
  cm->tile_cols = 1 << cm->log2_tile_cols;
  cm->tile_rows = 1 << cm->log2_tile_rows;

  cm->tile_width = (mi_cols_aligned_to_sb(cm->mi_cols) >> cm->log2_tile_cols);
  cm->tile_height = (mi_cols_aligned_to_sb(cm->mi_rows) >> cm->log2_tile_rows);
  // round to integer multiples of 8
  cm->tile_width  = mi_cols_aligned_to_sb(cm->tile_width);
  cm->tile_height = mi_cols_aligned_to_sb(cm->tile_height);
#endif
}

static void init_buffer_indices(VP9_COMP *cpi) {
  cpi->lst_fb_idx = 0;
#if CONFIG_MULTI_REF
  cpi->lst2_fb_idx = 1;
  cpi->lst3_fb_idx = 2;
  cpi->lst4_fb_idx = 3;
  cpi->gld_fb_idx = 4;
  cpi->alt_fb_idx = 5;
#else  // CONFIG_MULTI_REF
  cpi->gld_fb_idx = 1;
  cpi->alt_fb_idx = 2;
#endif  // CONFIG_MULTI_REF
}

static void init_config(struct VP9_COMP *cpi, VP9EncoderConfig *oxcf) {
  VP9_COMMON *const cm = &cpi->common;

  cpi->oxcf = *oxcf;
  cpi->framerate = oxcf->init_framerate;

  cm->profile = oxcf->profile;
  cm->bit_depth = oxcf->bit_depth;
#if CONFIG_VP9_HIGHBITDEPTH
  cm->use_highbitdepth = oxcf->use_highbitdepth;
#endif
  cm->color_space = oxcf->color_space;

  cm->width = oxcf->width;
  cm->height = oxcf->height;
  vp9_alloc_compressor_data(cpi);

  // change includes all joint functionality
  vp9_change_config(cpi, oxcf);

  cpi->static_mb_pct = 0;
  cpi->ref_frame_flags = 0;

  init_buffer_indices(cpi);
}

static void set_rc_buffer_sizes(RATE_CONTROL *rc,
                                const VP9EncoderConfig *oxcf) {
  const int64_t bandwidth = oxcf->target_bandwidth;
  const int64_t starting = oxcf->starting_buffer_level_ms;
  const int64_t optimal = oxcf->optimal_buffer_level_ms;
  const int64_t maximum = oxcf->maximum_buffer_size_ms;

  rc->starting_buffer_level = starting * bandwidth / 1000;
  rc->optimal_buffer_level = (optimal == 0) ? bandwidth / 8
                                            : optimal * bandwidth / 1000;
  rc->maximum_buffer_size = (maximum == 0) ? bandwidth / 8
                                           : maximum * bandwidth / 1000;
}

#if CONFIG_VP9_HIGHBITDEPTH
#define HIGHBD_BFP(BT, SDF, SDAF, VF, SVF, SVAF, SDX3F, SDX8F, SDX4DF) \
    cpi->fn_ptr[BT].sdf = SDF; \
    cpi->fn_ptr[BT].sdaf = SDAF; \
    cpi->fn_ptr[BT].vf = VF; \
    cpi->fn_ptr[BT].svf = SVF; \
    cpi->fn_ptr[BT].svaf = SVAF; \
    cpi->fn_ptr[BT].sdx3f = SDX3F; \
    cpi->fn_ptr[BT].sdx8f = SDX8F; \
    cpi->fn_ptr[BT].sdx4df = SDX4DF;

#define MAKE_BFP_SAD_WRAPPER(fnname) \
static unsigned int fnname##_bits8(const uint8_t *src_ptr, \
                                   int source_stride, \
                                   const uint8_t *ref_ptr, \
                                   int ref_stride) {  \
  return fnname(src_ptr, source_stride, ref_ptr, ref_stride); \
} \
static unsigned int fnname##_bits10(const uint8_t *src_ptr, \
                                    int source_stride, \
                                    const uint8_t *ref_ptr, \
                                    int ref_stride) {  \
  return fnname(src_ptr, source_stride, ref_ptr, ref_stride) >> 2; \
} \
static unsigned int fnname##_bits12(const uint8_t *src_ptr, \
                                    int source_stride, \
                                    const uint8_t *ref_ptr, \
                                    int ref_stride) {  \
  return fnname(src_ptr, source_stride, ref_ptr, ref_stride) >> 4; \
}

#define MAKE_BFP_SADAVG_WRAPPER(fnname) static unsigned int \
fnname##_bits8(const uint8_t *src_ptr, \
               int source_stride, \
               const uint8_t *ref_ptr, \
               int ref_stride, \
               const uint8_t *second_pred) {  \
  return fnname(src_ptr, source_stride, ref_ptr, ref_stride, second_pred); \
} \
static unsigned int fnname##_bits10(const uint8_t *src_ptr, \
                                    int source_stride, \
                                    const uint8_t *ref_ptr, \
                                    int ref_stride, \
                                    const uint8_t *second_pred) {  \
  return fnname(src_ptr, source_stride, ref_ptr, ref_stride, \
                second_pred) >> 2; \
} \
static unsigned int fnname##_bits12(const uint8_t *src_ptr, \
                                    int source_stride, \
                                    const uint8_t *ref_ptr, \
                                    int ref_stride, \
                                    const uint8_t *second_pred) {  \
  return fnname(src_ptr, source_stride, ref_ptr, ref_stride, \
                second_pred) >> 4; \
}

#define MAKE_BFP_SAD3_WRAPPER(fnname) \
static void fnname##_bits8(const uint8_t *src_ptr, \
                           int source_stride, \
                           const uint8_t *ref_ptr, \
                           int  ref_stride, \
                           unsigned int *sad_array) {  \
  fnname(src_ptr, source_stride, ref_ptr, ref_stride, sad_array); \
} \
static void fnname##_bits10(const uint8_t *src_ptr, \
                            int source_stride, \
                            const uint8_t *ref_ptr, \
                            int  ref_stride, \
                            unsigned int *sad_array) {  \
  int i; \
  fnname(src_ptr, source_stride, ref_ptr, ref_stride, sad_array); \
  for (i = 0; i < 3; i++) \
    sad_array[i] >>= 2; \
} \
static void fnname##_bits12(const uint8_t *src_ptr, \
                            int source_stride, \
                            const uint8_t *ref_ptr, \
                            int  ref_stride, \
                            unsigned int *sad_array) {  \
  int i; \
  fnname(src_ptr, source_stride, ref_ptr, ref_stride, sad_array); \
  for (i = 0; i < 3; i++) \
    sad_array[i] >>= 4; \
}

#define MAKE_BFP_SAD8_WRAPPER(fnname) \
static void fnname##_bits8(const uint8_t *src_ptr, \
                           int source_stride, \
                           const uint8_t *ref_ptr, \
                           int  ref_stride, \
                           unsigned int *sad_array) {  \
  fnname(src_ptr, source_stride, ref_ptr, ref_stride, sad_array); \
} \
static void fnname##_bits10(const uint8_t *src_ptr, \
                            int source_stride, \
                            const uint8_t *ref_ptr, \
                            int  ref_stride, \
                            unsigned int *sad_array) {  \
  int i; \
  fnname(src_ptr, source_stride, ref_ptr, ref_stride, sad_array); \
  for (i = 0; i < 8; i++) \
    sad_array[i] >>= 2; \
} \
static void fnname##_bits12(const uint8_t *src_ptr, \
                            int source_stride, \
                            const uint8_t *ref_ptr, \
                            int  ref_stride, \
                            unsigned int *sad_array) {  \
  int i; \
  fnname(src_ptr, source_stride, ref_ptr, ref_stride, sad_array); \
  for (i = 0; i < 8; i++) \
    sad_array[i] >>= 4; \
}
#define MAKE_BFP_SAD4D_WRAPPER(fnname) \
static void fnname##_bits8(const uint8_t *src_ptr, \
                           int source_stride, \
                           const uint8_t* const ref_ptr[], \
                           int  ref_stride, \
                           unsigned int *sad_array) {  \
  fnname(src_ptr, source_stride, ref_ptr, ref_stride, sad_array); \
} \
static void fnname##_bits10(const uint8_t *src_ptr, \
                            int source_stride, \
                            const uint8_t* const ref_ptr[], \
                            int  ref_stride, \
                            unsigned int *sad_array) {  \
  int i; \
  fnname(src_ptr, source_stride, ref_ptr, ref_stride, sad_array); \
  for (i = 0; i < 4; i++) \
  sad_array[i] >>= 2; \
} \
static void fnname##_bits12(const uint8_t *src_ptr, \
                            int source_stride, \
                            const uint8_t* const ref_ptr[], \
                            int  ref_stride, \
                            unsigned int *sad_array) {  \
  int i; \
  fnname(src_ptr, source_stride, ref_ptr, ref_stride, sad_array); \
  for (i = 0; i < 4; i++) \
  sad_array[i] >>= 4; \
}

#if CONFIG_EXT_CODING_UNIT_SIZE
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad128x128)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad128x128_avg)
MAKE_BFP_SAD3_WRAPPER(vp9_highbd_sad128x128x3)
MAKE_BFP_SAD8_WRAPPER(vp9_highbd_sad128x128x8)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad128x128x4d)
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad128x64)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad128x64_avg)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad128x64x4d)
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad64x128)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad64x128_avg)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad64x128x4d)
#endif
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad32x16)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad32x16_avg)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad32x16x4d)
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad16x32)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad16x32_avg)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad16x32x4d)
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad64x32)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad64x32_avg)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad64x32x4d)
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad32x64)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad32x64_avg)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad32x64x4d)
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad32x32)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad32x32_avg)
MAKE_BFP_SAD3_WRAPPER(vp9_highbd_sad32x32x3)
MAKE_BFP_SAD8_WRAPPER(vp9_highbd_sad32x32x8)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad32x32x4d)
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad64x64)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad64x64_avg)
MAKE_BFP_SAD3_WRAPPER(vp9_highbd_sad64x64x3)
MAKE_BFP_SAD8_WRAPPER(vp9_highbd_sad64x64x8)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad64x64x4d)
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad16x16)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad16x16_avg)
MAKE_BFP_SAD3_WRAPPER(vp9_highbd_sad16x16x3)
MAKE_BFP_SAD8_WRAPPER(vp9_highbd_sad16x16x8)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad16x16x4d)
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad16x8)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad16x8_avg)
MAKE_BFP_SAD3_WRAPPER(vp9_highbd_sad16x8x3)
MAKE_BFP_SAD8_WRAPPER(vp9_highbd_sad16x8x8)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad16x8x4d)
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad8x16)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad8x16_avg)
MAKE_BFP_SAD3_WRAPPER(vp9_highbd_sad8x16x3)
MAKE_BFP_SAD8_WRAPPER(vp9_highbd_sad8x16x8)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad8x16x4d)
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad8x8)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad8x8_avg)
MAKE_BFP_SAD3_WRAPPER(vp9_highbd_sad8x8x3)
MAKE_BFP_SAD8_WRAPPER(vp9_highbd_sad8x8x8)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad8x8x4d)
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad8x4)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad8x4_avg)
MAKE_BFP_SAD8_WRAPPER(vp9_highbd_sad8x4x8)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad8x4x4d)
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad4x8)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad4x8_avg)
MAKE_BFP_SAD8_WRAPPER(vp9_highbd_sad4x8x8)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad4x8x4d)
MAKE_BFP_SAD_WRAPPER(vp9_highbd_sad4x4)
MAKE_BFP_SADAVG_WRAPPER(vp9_highbd_sad4x4_avg)
MAKE_BFP_SAD3_WRAPPER(vp9_highbd_sad4x4x3)
MAKE_BFP_SAD8_WRAPPER(vp9_highbd_sad4x4x8)
MAKE_BFP_SAD4D_WRAPPER(vp9_highbd_sad4x4x4d)

#if CONFIG_WEDGE_PARTITION
#define HIGHBD_MBFP(BT, MSDF, MVF, MSVF)         \
  cpi->fn_ptr[BT].msdf            = MSDF; \
  cpi->fn_ptr[BT].mvf             = MVF;  \
  cpi->fn_ptr[BT].msvf            = MSVF;

#define MAKE_MBFP_SAD_WRAPPER(fnname) \
static unsigned int fnname##_bits8(const uint8_t *src_ptr, \
                                   int source_stride, \
                                   const uint8_t *ref_ptr, \
                                   int ref_stride, \
                                   const uint8_t *m, \
                                   int m_stride) {  \
  return fnname(src_ptr, source_stride, ref_ptr, ref_stride, \
                m, m_stride); \
} \
static unsigned int fnname##_bits10(const uint8_t *src_ptr, \
                                    int source_stride, \
                                    const uint8_t *ref_ptr, \
                                    int ref_stride, \
                                    const uint8_t *m, \
                                    int m_stride) {  \
  return fnname(src_ptr, source_stride, ref_ptr, ref_stride, \
                m, m_stride) >> 2; \
} \
static unsigned int fnname##_bits12(const uint8_t *src_ptr, \
                                    int source_stride, \
                                    const uint8_t *ref_ptr, \
                                    int ref_stride, \
                                    const uint8_t *m, \
                                    int m_stride) {  \
  return fnname(src_ptr, source_stride, ref_ptr, ref_stride, \
                m, m_stride) >> 4; \
}

#if CONFIG_EXT_CODING_UNIT_SIZE
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad128x128)
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad128x64)
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad64x128)
#endif
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad64x64)
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad64x32)
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad32x64)
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad32x32)
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad32x16)
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad16x32)
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad16x16)
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad16x8)
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad8x16)
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad8x8)
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad8x4)
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad4x8)
MAKE_MBFP_SAD_WRAPPER(vp9_highbd_masked_sad4x4)
#endif  // CONFIG_WEDGE_PARTITION

static void  highbd_set_var_fns(VP9_COMP *const cpi) {
  VP9_COMMON *const cm = &cpi->common;
  if (cm->use_highbitdepth) {
    switch (cm->bit_depth) {
      case VPX_BITS_8:
        HIGHBD_BFP(BLOCK_32X16,
                   vp9_highbd_sad32x16_bits8,
                   vp9_highbd_sad32x16_avg_bits8,
                   vp9_highbd_variance32x16,
                   vp9_highbd_sub_pixel_variance32x16,
                   vp9_highbd_sub_pixel_avg_variance32x16,
                   NULL,
                   NULL,
                   vp9_highbd_sad32x16x4d_bits8)

        HIGHBD_BFP(BLOCK_16X32,
                   vp9_highbd_sad16x32_bits8,
                   vp9_highbd_sad16x32_avg_bits8,
                   vp9_highbd_variance16x32,
                   vp9_highbd_sub_pixel_variance16x32,
                   vp9_highbd_sub_pixel_avg_variance16x32,
                   NULL,
                   NULL,
                   vp9_highbd_sad16x32x4d_bits8)

        HIGHBD_BFP(BLOCK_64X32,
                   vp9_highbd_sad64x32_bits8,
                   vp9_highbd_sad64x32_avg_bits8,
                   vp9_highbd_variance64x32,
                   vp9_highbd_sub_pixel_variance64x32,
                   vp9_highbd_sub_pixel_avg_variance64x32,
                   NULL,
                   NULL,
                   vp9_highbd_sad64x32x4d_bits8)

        HIGHBD_BFP(BLOCK_32X64,
                   vp9_highbd_sad32x64_bits8,
                   vp9_highbd_sad32x64_avg_bits8,
                   vp9_highbd_variance32x64,
                   vp9_highbd_sub_pixel_variance32x64,
                   vp9_highbd_sub_pixel_avg_variance32x64,
                   NULL,
                   NULL,
                   vp9_highbd_sad32x64x4d_bits8)

        HIGHBD_BFP(BLOCK_32X32,
                   vp9_highbd_sad32x32_bits8,
                   vp9_highbd_sad32x32_avg_bits8,
                   vp9_highbd_variance32x32,
                   vp9_highbd_sub_pixel_variance32x32,
                   vp9_highbd_sub_pixel_avg_variance32x32,
                   vp9_highbd_sad32x32x3_bits8,
                   vp9_highbd_sad32x32x8_bits8,
                   vp9_highbd_sad32x32x4d_bits8)

        HIGHBD_BFP(BLOCK_64X64,
                   vp9_highbd_sad64x64_bits8,
                   vp9_highbd_sad64x64_avg_bits8,
                   vp9_highbd_variance64x64,
                   vp9_highbd_sub_pixel_variance64x64,
                   vp9_highbd_sub_pixel_avg_variance64x64,
                   vp9_highbd_sad64x64x3_bits8,
                   vp9_highbd_sad64x64x8_bits8,
                   vp9_highbd_sad64x64x4d_bits8)

        HIGHBD_BFP(BLOCK_16X16,
                   vp9_highbd_sad16x16_bits8,
                   vp9_highbd_sad16x16_avg_bits8,
                   vp9_highbd_variance16x16,
                   vp9_highbd_sub_pixel_variance16x16,
                   vp9_highbd_sub_pixel_avg_variance16x16,
                   vp9_highbd_sad16x16x3_bits8,
                   vp9_highbd_sad16x16x8_bits8,
                   vp9_highbd_sad16x16x4d_bits8)

        HIGHBD_BFP(BLOCK_16X8,
                   vp9_highbd_sad16x8_bits8,
                   vp9_highbd_sad16x8_avg_bits8,
                   vp9_highbd_variance16x8,
                   vp9_highbd_sub_pixel_variance16x8,
                   vp9_highbd_sub_pixel_avg_variance16x8,
                   vp9_highbd_sad16x8x3_bits8,
                   vp9_highbd_sad16x8x8_bits8,
                   vp9_highbd_sad16x8x4d_bits8)

        HIGHBD_BFP(BLOCK_8X16,
                   vp9_highbd_sad8x16_bits8,
                   vp9_highbd_sad8x16_avg_bits8,
                   vp9_highbd_variance8x16,
                   vp9_highbd_sub_pixel_variance8x16,
                   vp9_highbd_sub_pixel_avg_variance8x16,
                   vp9_highbd_sad8x16x3_bits8,
                   vp9_highbd_sad8x16x8_bits8,
                   vp9_highbd_sad8x16x4d_bits8)

        HIGHBD_BFP(BLOCK_8X8,
                   vp9_highbd_sad8x8_bits8,
                   vp9_highbd_sad8x8_avg_bits8,
                   vp9_highbd_variance8x8,
                   vp9_highbd_sub_pixel_variance8x8,
                   vp9_highbd_sub_pixel_avg_variance8x8,
                   vp9_highbd_sad8x8x3_bits8,
                   vp9_highbd_sad8x8x8_bits8,
                   vp9_highbd_sad8x8x4d_bits8)

        HIGHBD_BFP(BLOCK_8X4,
                   vp9_highbd_sad8x4_bits8,
                   vp9_highbd_sad8x4_avg_bits8,
                   vp9_highbd_variance8x4,
                   vp9_highbd_sub_pixel_variance8x4,
                   vp9_highbd_sub_pixel_avg_variance8x4,
                   NULL,
                   vp9_highbd_sad8x4x8_bits8,
                   vp9_highbd_sad8x4x4d_bits8)

        HIGHBD_BFP(BLOCK_4X8,
                   vp9_highbd_sad4x8_bits8,
                   vp9_highbd_sad4x8_avg_bits8,
                   vp9_highbd_variance4x8,
                   vp9_highbd_sub_pixel_variance4x8,
                   vp9_highbd_sub_pixel_avg_variance4x8,
                   NULL,
                   vp9_highbd_sad4x8x8_bits8,
                   vp9_highbd_sad4x8x4d_bits8)

        HIGHBD_BFP(BLOCK_4X4,
                   vp9_highbd_sad4x4_bits8,
                   vp9_highbd_sad4x4_avg_bits8,
                   vp9_highbd_variance4x4,
                   vp9_highbd_sub_pixel_variance4x4,
                   vp9_highbd_sub_pixel_avg_variance4x4,
                   vp9_highbd_sad4x4x3_bits8,
                   vp9_highbd_sad4x4x8_bits8,
                   vp9_highbd_sad4x4x4d_bits8)

#if CONFIG_EXT_CODING_UNIT_SIZE
        HIGHBD_BFP(BLOCK_128X128,
                   vp9_highbd_sad128x128_bits8,
                   vp9_highbd_sad128x128_avg_bits8,
                   vp9_highbd_variance128x128,
                   vp9_highbd_sub_pixel_variance128x128,
                   vp9_highbd_sub_pixel_avg_variance128x128,
                   vp9_highbd_sad128x128x3_bits8,
                   vp9_highbd_sad128x128x8_bits8,
                   vp9_highbd_sad128x128x4d_bits8)

        HIGHBD_BFP(BLOCK_128X64,
                   vp9_highbd_sad128x64_bits8,
                   vp9_highbd_sad128x64_avg_bits8,
                   vp9_highbd_variance128x64,
                   vp9_highbd_sub_pixel_variance128x64,
                   vp9_highbd_sub_pixel_avg_variance128x64,
                   NULL,
                   NULL,
                   vp9_highbd_sad128x64x4d_bits8)

        HIGHBD_BFP(BLOCK_64X128,
                   vp9_highbd_sad64x128_bits8,
                   vp9_highbd_sad64x128_avg_bits8,
                   vp9_highbd_variance64x128,
                   vp9_highbd_sub_pixel_variance64x128,
                   vp9_highbd_sub_pixel_avg_variance64x128,
                   NULL,
                   NULL,
                   vp9_highbd_sad64x128x4d_bits8)
#endif

#if CONFIG_WEDGE_PARTITION
#if CONFIG_EXT_CODING_UNIT_SIZE
        HIGHBD_MBFP(BLOCK_128X128,
                    vp9_highbd_masked_sad128x128_bits8,
                    vp9_highbd_masked_variance128x128,
                    vp9_highbd_masked_sub_pixel_variance128x128)
        HIGHBD_MBFP(BLOCK_128X64,
                    vp9_highbd_masked_sad128x64_bits8,
                    vp9_highbd_masked_variance128x64,
                    vp9_highbd_masked_sub_pixel_variance128x64)
        HIGHBD_MBFP(BLOCK_64X128,
                    vp9_highbd_masked_sad64x128_bits8,
                    vp9_highbd_masked_variance64x128,
                    vp9_highbd_masked_sub_pixel_variance64x128)
#endif  // CONFIG_EXT_CODING_UNIT_SIZE
        HIGHBD_MBFP(BLOCK_64X64,
                    vp9_highbd_masked_sad64x64_bits8,
                    vp9_highbd_masked_variance64x64,
                    vp9_highbd_masked_sub_pixel_variance64x64)
        HIGHBD_MBFP(BLOCK_64X32,
                    vp9_highbd_masked_sad64x32_bits8,
                    vp9_highbd_masked_variance64x32,
                    vp9_highbd_masked_sub_pixel_variance64x32)
        HIGHBD_MBFP(BLOCK_32X64,
                    vp9_highbd_masked_sad32x64_bits8,
                    vp9_highbd_masked_variance32x64,
                    vp9_highbd_masked_sub_pixel_variance32x64)
        HIGHBD_MBFP(BLOCK_32X32,
                    vp9_highbd_masked_sad32x32_bits8,
                    vp9_highbd_masked_variance32x32,
                    vp9_highbd_masked_sub_pixel_variance32x32)
        HIGHBD_MBFP(BLOCK_32X16,
                    vp9_highbd_masked_sad32x16_bits8,
                    vp9_highbd_masked_variance32x16,
                    vp9_highbd_masked_sub_pixel_variance32x16)
        HIGHBD_MBFP(BLOCK_16X32,
                    vp9_highbd_masked_sad16x32_bits8,
                    vp9_highbd_masked_variance16x32,
                    vp9_highbd_masked_sub_pixel_variance16x32)
        HIGHBD_MBFP(BLOCK_16X16,
                    vp9_highbd_masked_sad16x16_bits8,
                    vp9_highbd_masked_variance16x16,
                    vp9_highbd_masked_sub_pixel_variance16x16)
        HIGHBD_MBFP(BLOCK_8X16,
                    vp9_highbd_masked_sad8x16_bits8,
                    vp9_highbd_masked_variance8x16,
                    vp9_highbd_masked_sub_pixel_variance8x16)
        HIGHBD_MBFP(BLOCK_16X8,
                    vp9_highbd_masked_sad16x8_bits8,
                    vp9_highbd_masked_variance16x8,
                    vp9_highbd_masked_sub_pixel_variance16x8)
        HIGHBD_MBFP(BLOCK_8X8,
                    vp9_highbd_masked_sad8x8_bits8,
                    vp9_highbd_masked_variance8x8,
                    vp9_highbd_masked_sub_pixel_variance8x8)
        HIGHBD_MBFP(BLOCK_4X8,
                    vp9_highbd_masked_sad4x8_bits8,
                    vp9_highbd_masked_variance4x8,
                    vp9_highbd_masked_sub_pixel_variance4x8)
        HIGHBD_MBFP(BLOCK_8X4,
                    vp9_highbd_masked_sad8x4_bits8,
                    vp9_highbd_masked_variance8x4,
                    vp9_highbd_masked_sub_pixel_variance8x4)
        HIGHBD_MBFP(BLOCK_4X4,
                    vp9_highbd_masked_sad4x4_bits8,
                    vp9_highbd_masked_variance4x4,
                    vp9_highbd_masked_sub_pixel_variance4x4)
#endif  // CONFIG_WEDGE_PARTITION
        break;

      case VPX_BITS_10:
        HIGHBD_BFP(BLOCK_32X16,
                   vp9_highbd_sad32x16_bits10,
                   vp9_highbd_sad32x16_avg_bits10,
                   vp9_highbd_10_variance32x16,
                   vp9_highbd_10_sub_pixel_variance32x16,
                   vp9_highbd_10_sub_pixel_avg_variance32x16,
                   NULL,
                   NULL,
                   vp9_highbd_sad32x16x4d_bits10)

        HIGHBD_BFP(BLOCK_16X32,
                   vp9_highbd_sad16x32_bits10,
                   vp9_highbd_sad16x32_avg_bits10,
                   vp9_highbd_10_variance16x32,
                   vp9_highbd_10_sub_pixel_variance16x32,
                   vp9_highbd_10_sub_pixel_avg_variance16x32,
                   NULL,
                   NULL,
                   vp9_highbd_sad16x32x4d_bits10)

        HIGHBD_BFP(BLOCK_64X32,
                   vp9_highbd_sad64x32_bits10,
                   vp9_highbd_sad64x32_avg_bits10,
                   vp9_highbd_10_variance64x32,
                   vp9_highbd_10_sub_pixel_variance64x32,
                   vp9_highbd_10_sub_pixel_avg_variance64x32,
                   NULL,
                   NULL,
                   vp9_highbd_sad64x32x4d_bits10)

        HIGHBD_BFP(BLOCK_32X64,
                   vp9_highbd_sad32x64_bits10,
                   vp9_highbd_sad32x64_avg_bits10,
                   vp9_highbd_10_variance32x64,
                   vp9_highbd_10_sub_pixel_variance32x64,
                   vp9_highbd_10_sub_pixel_avg_variance32x64,
                   NULL,
                   NULL,
                   vp9_highbd_sad32x64x4d_bits10)

        HIGHBD_BFP(BLOCK_32X32,
                   vp9_highbd_sad32x32_bits10,
                   vp9_highbd_sad32x32_avg_bits10,
                   vp9_highbd_10_variance32x32,
                   vp9_highbd_10_sub_pixel_variance32x32,
                   vp9_highbd_10_sub_pixel_avg_variance32x32,
                   vp9_highbd_sad32x32x3_bits10,
                   vp9_highbd_sad32x32x8_bits10,
                   vp9_highbd_sad32x32x4d_bits10)

        HIGHBD_BFP(BLOCK_64X64,
                   vp9_highbd_sad64x64_bits10,
                   vp9_highbd_sad64x64_avg_bits10,
                   vp9_highbd_10_variance64x64,
                   vp9_highbd_10_sub_pixel_variance64x64,
                   vp9_highbd_10_sub_pixel_avg_variance64x64,
                   vp9_highbd_sad64x64x3_bits10,
                   vp9_highbd_sad64x64x8_bits10,
                   vp9_highbd_sad64x64x4d_bits10)

        HIGHBD_BFP(BLOCK_16X16,
                   vp9_highbd_sad16x16_bits10,
                   vp9_highbd_sad16x16_avg_bits10,
                   vp9_highbd_10_variance16x16,
                   vp9_highbd_10_sub_pixel_variance16x16,
                   vp9_highbd_10_sub_pixel_avg_variance16x16,
                   vp9_highbd_sad16x16x3_bits10,
                   vp9_highbd_sad16x16x8_bits10,
                   vp9_highbd_sad16x16x4d_bits10)

        HIGHBD_BFP(BLOCK_16X8,
                   vp9_highbd_sad16x8_bits10,
                   vp9_highbd_sad16x8_avg_bits10,
                   vp9_highbd_10_variance16x8,
                   vp9_highbd_10_sub_pixel_variance16x8,
                   vp9_highbd_10_sub_pixel_avg_variance16x8,
                   vp9_highbd_sad16x8x3_bits10,
                   vp9_highbd_sad16x8x8_bits10,
                   vp9_highbd_sad16x8x4d_bits10)

        HIGHBD_BFP(BLOCK_8X16,
                   vp9_highbd_sad8x16_bits10,
                   vp9_highbd_sad8x16_avg_bits10,
                   vp9_highbd_10_variance8x16,
                   vp9_highbd_10_sub_pixel_variance8x16,
                   vp9_highbd_10_sub_pixel_avg_variance8x16,
                   vp9_highbd_sad8x16x3_bits10,
                   vp9_highbd_sad8x16x8_bits10,
                   vp9_highbd_sad8x16x4d_bits10)

        HIGHBD_BFP(BLOCK_8X8,
                   vp9_highbd_sad8x8_bits10,
                   vp9_highbd_sad8x8_avg_bits10,
                   vp9_highbd_10_variance8x8,
                   vp9_highbd_10_sub_pixel_variance8x8,
                   vp9_highbd_10_sub_pixel_avg_variance8x8,
                   vp9_highbd_sad8x8x3_bits10,
                   vp9_highbd_sad8x8x8_bits10,
                   vp9_highbd_sad8x8x4d_bits10)

        HIGHBD_BFP(BLOCK_8X4,
                   vp9_highbd_sad8x4_bits10,
                   vp9_highbd_sad8x4_avg_bits10,
                   vp9_highbd_10_variance8x4,
                   vp9_highbd_10_sub_pixel_variance8x4,
                   vp9_highbd_10_sub_pixel_avg_variance8x4,
                   NULL,
                   vp9_highbd_sad8x4x8_bits10,
                   vp9_highbd_sad8x4x4d_bits10)

        HIGHBD_BFP(BLOCK_4X8,
                   vp9_highbd_sad4x8_bits10,
                   vp9_highbd_sad4x8_avg_bits10,
                   vp9_highbd_10_variance4x8,
                   vp9_highbd_10_sub_pixel_variance4x8,
                   vp9_highbd_10_sub_pixel_avg_variance4x8,
                   NULL,
                   vp9_highbd_sad4x8x8_bits10,
                   vp9_highbd_sad4x8x4d_bits10)

        HIGHBD_BFP(BLOCK_4X4,
                   vp9_highbd_sad4x4_bits10,
                   vp9_highbd_sad4x4_avg_bits10,
                   vp9_highbd_10_variance4x4,
                   vp9_highbd_10_sub_pixel_variance4x4,
                   vp9_highbd_10_sub_pixel_avg_variance4x4,
                   vp9_highbd_sad4x4x3_bits10,
                   vp9_highbd_sad4x4x8_bits10,
                   vp9_highbd_sad4x4x4d_bits10)

#if CONFIG_EXT_CODING_UNIT_SIZE
        HIGHBD_BFP(BLOCK_128X128,
                   vp9_highbd_sad128x128_bits10,
                   vp9_highbd_sad128x128_avg_bits10,
                   vp9_highbd_10_variance128x128,
                   vp9_highbd_10_sub_pixel_variance128x128,
                   vp9_highbd_10_sub_pixel_avg_variance128x128,
                   vp9_highbd_sad128x128x3_bits10,
                   vp9_highbd_sad128x128x8_bits10,
                   vp9_highbd_sad128x128x4d_bits10)

        HIGHBD_BFP(BLOCK_128X64,
                   vp9_highbd_sad128x64_bits10,
                   vp9_highbd_sad128x64_avg_bits10,
                   vp9_highbd_10_variance128x64,
                   vp9_highbd_10_sub_pixel_variance128x64,
                   vp9_highbd_10_sub_pixel_avg_variance128x64,
                   NULL,
                   NULL,
                   vp9_highbd_sad128x64x4d_bits10)

        HIGHBD_BFP(BLOCK_64X128,
                   vp9_highbd_sad64x128_bits10,
                   vp9_highbd_sad64x128_avg_bits10,
                   vp9_highbd_10_variance64x128,
                   vp9_highbd_10_sub_pixel_variance64x128,
                   vp9_highbd_10_sub_pixel_avg_variance64x128,
                   NULL,
                   NULL,
                   vp9_highbd_sad64x128x4d_bits10)
#endif

#if CONFIG_WEDGE_PARTITION
#if CONFIG_EXT_CODING_UNIT_SIZE
        HIGHBD_MBFP(BLOCK_128X128,
                    vp9_highbd_masked_sad128x128_bits10,
                    vp9_highbd_10_masked_variance128x128,
                    vp9_highbd_10_masked_sub_pixel_variance128x128)
        HIGHBD_MBFP(BLOCK_128X64,
                    vp9_highbd_masked_sad128x64_bits10,
                    vp9_highbd_10_masked_variance128x64,
                    vp9_highbd_10_masked_sub_pixel_variance128x64)
        HIGHBD_MBFP(BLOCK_64X128,
                    vp9_highbd_masked_sad64x128_bits10,
                    vp9_highbd_10_masked_variance64x128,
                    vp9_highbd_10_masked_sub_pixel_variance64x128)
#endif  // CONFIG_EXT_CODING_UNIT_SIZE
        HIGHBD_MBFP(BLOCK_64X64,
                    vp9_highbd_masked_sad64x64_bits10,
                    vp9_highbd_10_masked_variance64x64,
                    vp9_highbd_10_masked_sub_pixel_variance64x64)
        HIGHBD_MBFP(BLOCK_64X32,
                    vp9_highbd_masked_sad64x32_bits10,
                    vp9_highbd_10_masked_variance64x32,
                    vp9_highbd_10_masked_sub_pixel_variance64x32)
        HIGHBD_MBFP(BLOCK_32X64,
                    vp9_highbd_masked_sad32x64_bits10,
                    vp9_highbd_10_masked_variance32x64,
                    vp9_highbd_10_masked_sub_pixel_variance32x64)
        HIGHBD_MBFP(BLOCK_32X32,
                    vp9_highbd_masked_sad32x32_bits10,
                    vp9_highbd_10_masked_variance32x32,
                    vp9_highbd_10_masked_sub_pixel_variance32x32)
        HIGHBD_MBFP(BLOCK_32X16,
                    vp9_highbd_masked_sad32x16_bits10,
                    vp9_highbd_10_masked_variance32x16,
                    vp9_highbd_10_masked_sub_pixel_variance32x16)
        HIGHBD_MBFP(BLOCK_16X32,
                    vp9_highbd_masked_sad16x32_bits10,
                    vp9_highbd_10_masked_variance16x32,
                    vp9_highbd_10_masked_sub_pixel_variance16x32)
        HIGHBD_MBFP(BLOCK_16X16,
                    vp9_highbd_masked_sad16x16_bits10,
                    vp9_highbd_10_masked_variance16x16,
                    vp9_highbd_10_masked_sub_pixel_variance16x16)
        HIGHBD_MBFP(BLOCK_8X16,
                    vp9_highbd_masked_sad8x16_bits10,
                    vp9_highbd_10_masked_variance8x16,
                    vp9_highbd_10_masked_sub_pixel_variance8x16)
        HIGHBD_MBFP(BLOCK_16X8,
                    vp9_highbd_masked_sad16x8_bits10,
                    vp9_highbd_10_masked_variance16x8,
                    vp9_highbd_10_masked_sub_pixel_variance16x8)
        HIGHBD_MBFP(BLOCK_8X8,
                    vp9_highbd_masked_sad8x8_bits10,
                    vp9_highbd_10_masked_variance8x8,
                    vp9_highbd_10_masked_sub_pixel_variance8x8)
        HIGHBD_MBFP(BLOCK_4X8,
                    vp9_highbd_masked_sad4x8_bits10,
                    vp9_highbd_10_masked_variance4x8,
                    vp9_highbd_10_masked_sub_pixel_variance4x8)
        HIGHBD_MBFP(BLOCK_8X4,
                    vp9_highbd_masked_sad8x4_bits10,
                    vp9_highbd_10_masked_variance8x4,
                    vp9_highbd_10_masked_sub_pixel_variance8x4)
        HIGHBD_MBFP(BLOCK_4X4,
                    vp9_highbd_masked_sad4x4_bits10,
                    vp9_highbd_10_masked_variance4x4,
                    vp9_highbd_10_masked_sub_pixel_variance4x4)
#endif  // CONFIG_WEDGE_PARTITION
        break;

      case VPX_BITS_12:
        HIGHBD_BFP(BLOCK_32X16,
                   vp9_highbd_sad32x16_bits12,
                   vp9_highbd_sad32x16_avg_bits12,
                   vp9_highbd_12_variance32x16,
                   vp9_highbd_12_sub_pixel_variance32x16,
                   vp9_highbd_12_sub_pixel_avg_variance32x16,
                   NULL,
                   NULL,
                   vp9_highbd_sad32x16x4d_bits12)

        HIGHBD_BFP(BLOCK_16X32,
                   vp9_highbd_sad16x32_bits12,
                   vp9_highbd_sad16x32_avg_bits12,
                   vp9_highbd_12_variance16x32,
                   vp9_highbd_12_sub_pixel_variance16x32,
                   vp9_highbd_12_sub_pixel_avg_variance16x32,
                   NULL,
                   NULL,
                   vp9_highbd_sad16x32x4d_bits12)

        HIGHBD_BFP(BLOCK_64X32,
                   vp9_highbd_sad64x32_bits12,
                   vp9_highbd_sad64x32_avg_bits12,
                   vp9_highbd_12_variance64x32,
                   vp9_highbd_12_sub_pixel_variance64x32,
                   vp9_highbd_12_sub_pixel_avg_variance64x32,
                   NULL,
                   NULL,
                   vp9_highbd_sad64x32x4d_bits12)

        HIGHBD_BFP(BLOCK_32X64,
                   vp9_highbd_sad32x64_bits12,
                   vp9_highbd_sad32x64_avg_bits12,
                   vp9_highbd_12_variance32x64,
                   vp9_highbd_12_sub_pixel_variance32x64,
                   vp9_highbd_12_sub_pixel_avg_variance32x64,
                   NULL,
                   NULL,
                   vp9_highbd_sad32x64x4d_bits12)

        HIGHBD_BFP(BLOCK_32X32,
                   vp9_highbd_sad32x32_bits12,
                   vp9_highbd_sad32x32_avg_bits12,
                   vp9_highbd_12_variance32x32,
                   vp9_highbd_12_sub_pixel_variance32x32,
                   vp9_highbd_12_sub_pixel_avg_variance32x32,
                   vp9_highbd_sad32x32x3_bits12,
                   vp9_highbd_sad32x32x8_bits12,
                   vp9_highbd_sad32x32x4d_bits12)

        HIGHBD_BFP(BLOCK_64X64,
                   vp9_highbd_sad64x64_bits12,
                   vp9_highbd_sad64x64_avg_bits12,
                   vp9_highbd_12_variance64x64,
                   vp9_highbd_12_sub_pixel_variance64x64,
                   vp9_highbd_12_sub_pixel_avg_variance64x64,
                   vp9_highbd_sad64x64x3_bits12,
                   vp9_highbd_sad64x64x8_bits12,
                   vp9_highbd_sad64x64x4d_bits12)

        HIGHBD_BFP(BLOCK_16X16,
                   vp9_highbd_sad16x16_bits12,
                   vp9_highbd_sad16x16_avg_bits12,
                   vp9_highbd_12_variance16x16,
                   vp9_highbd_12_sub_pixel_variance16x16,
                   vp9_highbd_12_sub_pixel_avg_variance16x16,
                   vp9_highbd_sad16x16x3_bits12,
                   vp9_highbd_sad16x16x8_bits12,
                   vp9_highbd_sad16x16x4d_bits12)

        HIGHBD_BFP(BLOCK_16X8,
                   vp9_highbd_sad16x8_bits12,
                   vp9_highbd_sad16x8_avg_bits12,
                   vp9_highbd_12_variance16x8,
                   vp9_highbd_12_sub_pixel_variance16x8,
                   vp9_highbd_12_sub_pixel_avg_variance16x8,
                   vp9_highbd_sad16x8x3_bits12,
                   vp9_highbd_sad16x8x8_bits12,
                   vp9_highbd_sad16x8x4d_bits12)

        HIGHBD_BFP(BLOCK_8X16,
                   vp9_highbd_sad8x16_bits12,
                   vp9_highbd_sad8x16_avg_bits12,
                   vp9_highbd_12_variance8x16,
                   vp9_highbd_12_sub_pixel_variance8x16,
                   vp9_highbd_12_sub_pixel_avg_variance8x16,
                   vp9_highbd_sad8x16x3_bits12,
                   vp9_highbd_sad8x16x8_bits12,
                   vp9_highbd_sad8x16x4d_bits12)

        HIGHBD_BFP(BLOCK_8X8,
                   vp9_highbd_sad8x8_bits12,
                   vp9_highbd_sad8x8_avg_bits12,
                   vp9_highbd_12_variance8x8,
                   vp9_highbd_12_sub_pixel_variance8x8,
                   vp9_highbd_12_sub_pixel_avg_variance8x8,
                   vp9_highbd_sad8x8x3_bits12,
                   vp9_highbd_sad8x8x8_bits12,
                   vp9_highbd_sad8x8x4d_bits12)

        HIGHBD_BFP(BLOCK_8X4,
                   vp9_highbd_sad8x4_bits12,
                   vp9_highbd_sad8x4_avg_bits12,
                   vp9_highbd_12_variance8x4,
                   vp9_highbd_12_sub_pixel_variance8x4,
                   vp9_highbd_12_sub_pixel_avg_variance8x4,
                   NULL,
                   vp9_highbd_sad8x4x8_bits12,
                   vp9_highbd_sad8x4x4d_bits12)

        HIGHBD_BFP(BLOCK_4X8,
                   vp9_highbd_sad4x8_bits12,
                   vp9_highbd_sad4x8_avg_bits12,
                   vp9_highbd_12_variance4x8,
                   vp9_highbd_12_sub_pixel_variance4x8,
                   vp9_highbd_12_sub_pixel_avg_variance4x8,
                   NULL,
                   vp9_highbd_sad4x8x8_bits12,
                   vp9_highbd_sad4x8x4d_bits12)

        HIGHBD_BFP(BLOCK_4X4,
                   vp9_highbd_sad4x4_bits12,
                   vp9_highbd_sad4x4_avg_bits12,
                   vp9_highbd_12_variance4x4,
                   vp9_highbd_12_sub_pixel_variance4x4,
                   vp9_highbd_12_sub_pixel_avg_variance4x4,
                   vp9_highbd_sad4x4x3_bits12,
                   vp9_highbd_sad4x4x8_bits12,
                   vp9_highbd_sad4x4x4d_bits12)

#if CONFIG_EXT_CODING_UNIT_SIZE
        HIGHBD_BFP(BLOCK_128X128,
                   vp9_highbd_sad128x128_bits12,
                   vp9_highbd_sad128x128_avg_bits12,
                   vp9_highbd_12_variance128x128,
                   vp9_highbd_12_sub_pixel_variance128x128,
                   vp9_highbd_12_sub_pixel_avg_variance128x128,
                   vp9_highbd_sad128x128x3_bits12,
                   vp9_highbd_sad128x128x8_bits12,
                   vp9_highbd_sad128x128x4d_bits12)

        HIGHBD_BFP(BLOCK_128X64,
                   vp9_highbd_sad128x64_bits12,
                   vp9_highbd_sad128x64_avg_bits12,
                   vp9_highbd_12_variance128x64,
                   vp9_highbd_12_sub_pixel_variance128x64,
                   vp9_highbd_12_sub_pixel_avg_variance128x64,
                   NULL,
                   NULL,
                   vp9_highbd_sad128x64x4d_bits12)

        HIGHBD_BFP(BLOCK_64X128,
                   vp9_highbd_sad64x128_bits12,
                   vp9_highbd_sad64x128_avg_bits12,
                   vp9_highbd_12_variance64x128,
                   vp9_highbd_12_sub_pixel_variance64x128,
                   vp9_highbd_12_sub_pixel_avg_variance64x128,
                   NULL,
                   NULL,
                   vp9_highbd_sad64x128x4d_bits12)
#endif

#if CONFIG_WEDGE_PARTITION
#if CONFIG_EXT_CODING_UNIT_SIZE
        HIGHBD_MBFP(BLOCK_128X128,
                    vp9_highbd_masked_sad128x128_bits12,
                    vp9_highbd_12_masked_variance128x128,
                    vp9_highbd_12_masked_sub_pixel_variance128x128)
        HIGHBD_MBFP(BLOCK_128X64,
                    vp9_highbd_masked_sad128x64_bits12,
                    vp9_highbd_12_masked_variance128x64,
                    vp9_highbd_12_masked_sub_pixel_variance128x64)
        HIGHBD_MBFP(BLOCK_64X128,
                    vp9_highbd_masked_sad64x128_bits12,
                    vp9_highbd_12_masked_variance64x128,
                    vp9_highbd_12_masked_sub_pixel_variance64x128)
#endif  // CONFIG_EXT_CODING_UNIT_SIZE
        HIGHBD_MBFP(BLOCK_64X64,
                    vp9_highbd_masked_sad64x64_bits12,
                    vp9_highbd_12_masked_variance64x64,
                    vp9_highbd_12_masked_sub_pixel_variance64x64)
        HIGHBD_MBFP(BLOCK_64X32,
                    vp9_highbd_masked_sad64x32_bits12,
                    vp9_highbd_12_masked_variance64x32,
                    vp9_highbd_12_masked_sub_pixel_variance64x32)
        HIGHBD_MBFP(BLOCK_32X64,
                    vp9_highbd_masked_sad32x64_bits12,
                    vp9_highbd_12_masked_variance32x64,
                    vp9_highbd_12_masked_sub_pixel_variance32x64)
        HIGHBD_MBFP(BLOCK_32X32,
                    vp9_highbd_masked_sad32x32_bits12,
                    vp9_highbd_12_masked_variance32x32,
                    vp9_highbd_12_masked_sub_pixel_variance32x32)
        HIGHBD_MBFP(BLOCK_32X16,
                    vp9_highbd_masked_sad32x16_bits12,
                    vp9_highbd_12_masked_variance32x16,
                    vp9_highbd_12_masked_sub_pixel_variance32x16)
        HIGHBD_MBFP(BLOCK_16X32,
                    vp9_highbd_masked_sad16x32_bits12,
                    vp9_highbd_12_masked_variance16x32,
                    vp9_highbd_12_masked_sub_pixel_variance16x32)
        HIGHBD_MBFP(BLOCK_16X16,
                    vp9_highbd_masked_sad16x16_bits12,
                    vp9_highbd_12_masked_variance16x16,
                    vp9_highbd_12_masked_sub_pixel_variance16x16)
        HIGHBD_MBFP(BLOCK_8X16,
                    vp9_highbd_masked_sad8x16_bits12,
                    vp9_highbd_12_masked_variance8x16,
                    vp9_highbd_12_masked_sub_pixel_variance8x16)
        HIGHBD_MBFP(BLOCK_16X8,
                    vp9_highbd_masked_sad16x8_bits12,
                    vp9_highbd_12_masked_variance16x8,
                    vp9_highbd_12_masked_sub_pixel_variance16x8)
        HIGHBD_MBFP(BLOCK_8X8,
                    vp9_highbd_masked_sad8x8_bits12,
                    vp9_highbd_12_masked_variance8x8,
                    vp9_highbd_12_masked_sub_pixel_variance8x8)
        HIGHBD_MBFP(BLOCK_4X8,
                    vp9_highbd_masked_sad4x8_bits12,
                    vp9_highbd_12_masked_variance4x8,
                    vp9_highbd_12_masked_sub_pixel_variance4x8)
        HIGHBD_MBFP(BLOCK_8X4,
                    vp9_highbd_masked_sad8x4_bits12,
                    vp9_highbd_12_masked_variance8x4,
                    vp9_highbd_12_masked_sub_pixel_variance8x4)
        HIGHBD_MBFP(BLOCK_4X4,
                    vp9_highbd_masked_sad4x4_bits12,
                    vp9_highbd_12_masked_variance4x4,
                    vp9_highbd_12_masked_sub_pixel_variance4x4)
#endif  // CONFIG_WEDGE_PARTITION
        break;

      default:
        assert(0 && "cm->bit_depth should be VPX_BITS_8, "
                    "VPX_BITS_10 or VPX_BITS_12");
    }
  }
}
#endif  // CONFIG_VP9_HIGHBITDEPTH

void vp9_change_config(struct VP9_COMP *cpi, const VP9EncoderConfig *oxcf) {
  VP9_COMMON *const cm = &cpi->common;
  RATE_CONTROL *const rc = &cpi->rc;

  if (cm->profile != oxcf->profile)
    cm->profile = oxcf->profile;
  cm->bit_depth = oxcf->bit_depth;
  cm->color_space = oxcf->color_space;

  if (cm->profile <= PROFILE_1)
    assert(cm->bit_depth == VPX_BITS_8);
  else
    assert(cm->bit_depth > VPX_BITS_8);

  cpi->oxcf = *oxcf;
#if CONFIG_VP9_HIGHBITDEPTH
  cpi->mb.e_mbd.bd = (int)cm->bit_depth;
#endif  // CONFIG_VP9_HIGHBITDEPTH
#if CONFIG_GLOBAL_MOTION
  cpi->mb.e_mbd.global_motion = cm->global_motion;
#endif  // CONFIG_GLOBAL_MOTION

  rc->baseline_gf_interval = DEFAULT_GF_INTERVAL;

  cpi->refresh_golden_frame = 0;
  cpi->refresh_last_frame = 1;
#if CONFIG_MULTI_REF
  cpi->refresh_last2_frame = 0;
  cpi->refresh_last3_frame = 0;
  cpi->refresh_last4_frame = 0;
#endif  // CONFIG_MULTI_REF
  cm->refresh_frame_context = 1;
  cm->reset_frame_context = 0;

  vp9_reset_segment_features(&cm->seg);
  vp9_set_high_precision_mv(cpi, 0);

  {
    int i;

    for (i = 0; i < MAX_SEGMENTS; i++)
      cpi->segment_encode_breakout[i] = cpi->oxcf.encode_breakout;
  }
  cpi->encode_breakout = cpi->oxcf.encode_breakout;

  set_rc_buffer_sizes(rc, &cpi->oxcf);

  // Under a configuration change, where maximum_buffer_size may change,
  // keep buffer level clipped to the maximum allowed buffer size.
  rc->bits_off_target = MIN(rc->bits_off_target, rc->maximum_buffer_size);
  rc->buffer_level = MIN(rc->buffer_level, rc->maximum_buffer_size);

  // Set up frame rate and related parameters rate control values.
  vp9_new_framerate(cpi, cpi->framerate);

  // Set absolute upper and lower quality limits
  rc->worst_quality = cpi->oxcf.worst_allowed_q;
  rc->best_quality = cpi->oxcf.best_allowed_q;

  cm->interp_filter = cpi->sf.default_interp_filter;

  cm->display_width = cpi->oxcf.width;
  cm->display_height = cpi->oxcf.height;

  if (cpi->initial_width) {
    // Increasing the size of the frame beyond the first seen frame, or some
    // otherwise signaled maximum size, is not supported.
    // TODO(jkoleszar): exit gracefully.
    assert(cm->width <= cpi->initial_width);
    assert(cm->height <= cpi->initial_height);
  }
  update_frame_size(cpi);

  cpi->alt_ref_source = NULL;
  rc->is_src_frame_alt_ref = 0;

#if 0
  // Experimental RD Code
  cpi->frame_distortion = 0;
  cpi->last_frame_distortion = 0;
#endif

  set_tile_limits(cpi);

  cpi->ext_refresh_frame_flags_pending = 0;
  cpi->ext_refresh_frame_context_pending = 0;

#if CONFIG_VP9_HIGHBITDEPTH
  highbd_set_var_fns(cpi);
#endif

#if CONFIG_VP9_TEMPORAL_DENOISING
  if (cpi->oxcf.noise_sensitivity > 0) {
    vp9_denoiser_alloc(&(cpi->denoiser), cm->width, cm->height,
                       cm->subsampling_x, cm->subsampling_y,
#if CONFIG_VP9_HIGHBITDEPTH
                       cm->use_highbitdepth,
#endif
                       VP9_ENC_BORDER_IN_PIXELS);
  }
#endif
}

#ifndef M_LOG2_E
#define M_LOG2_E 0.693147180559945309417
#endif
#define log2f(x) (log (x) / (float) M_LOG2_E)

static void cal_nmvjointsadcost(int *mvjointsadcost) {
  mvjointsadcost[0] = 600;
  mvjointsadcost[1] = 300;
  mvjointsadcost[2] = 300;
  mvjointsadcost[3] = 300;
}

static void cal_nmvsadcosts(int *mvsadcost[2]) {
  int i = 1;

  mvsadcost[0][0] = 0;
  mvsadcost[1][0] = 0;

  do {
    double z = 256 * (2 * (log2f(8 * i) + .6));
    mvsadcost[0][i] = (int)z;
    mvsadcost[1][i] = (int)z;
    mvsadcost[0][-i] = (int)z;
    mvsadcost[1][-i] = (int)z;
  } while (++i <= MV_MAX);
}

static void cal_nmvsadcosts_hp(int *mvsadcost[2]) {
  int i = 1;

  mvsadcost[0][0] = 0;
  mvsadcost[1][0] = 0;

  do {
    double z = 256 * (2 * (log2f(8 * i) + .6));
    mvsadcost[0][i] = (int)z;
    mvsadcost[1][i] = (int)z;
    mvsadcost[0][-i] = (int)z;
    mvsadcost[1][-i] = (int)z;
  } while (++i <= MV_MAX);
}


VP9_COMP *vp9_create_compressor(VP9EncoderConfig *oxcf) {
  unsigned int i, j;
  VP9_COMP *const cpi = vpx_memalign(32, sizeof(VP9_COMP));
  VP9_COMMON *const cm = cpi != NULL ? &cpi->common : NULL;

  if (!cm)
    return NULL;

  vp9_zero(*cpi);

  if (setjmp(cm->error.jmp)) {
    cm->error.setjmp = 0;
    vp9_remove_compressor(cpi);
    return 0;
  }

  cm->error.setjmp = 1;

  init_config(cpi, oxcf);
  vp9_rc_init(&cpi->oxcf, oxcf->pass, &cpi->rc);

  cm->current_video_frame = 0;
  cpi->partition_search_skippable_frame = 0;

  // Create the encoder segmentation map and set all entries to 0
  CHECK_MEM_ERROR(cm, cpi->segmentation_map,
                  vpx_calloc(cm->mi_rows * cm->mi_cols, 1));

  // Create a complexity map used for rd adjustment
  CHECK_MEM_ERROR(cm, cpi->complexity_map,
                  vpx_calloc(cm->mi_rows * cm->mi_cols, 1));

  // Create a map used for cyclic background refresh.
  CHECK_MEM_ERROR(cm, cpi->cyclic_refresh,
                  vp9_cyclic_refresh_alloc(cm->mi_rows, cm->mi_cols));

  // And a place holder structure is the coding context
  // for use if we want to save and restore it
  CHECK_MEM_ERROR(cm, cpi->coding_context.last_frame_seg_map_copy,
                  vpx_calloc(cm->mi_rows * cm->mi_cols, 1));

#if CONFIG_INTRABC
  CHECK_MEM_ERROR(cm, cpi->ndvcosts[0],
                  vpx_calloc(MV_VALS, sizeof(*cpi->ndvcosts[0])));
  CHECK_MEM_ERROR(cm, cpi->ndvcosts[1],
                  vpx_calloc(MV_VALS, sizeof(*cpi->ndvcosts[1])));
#endif  // CONFIG_INTRABC
  CHECK_MEM_ERROR(cm, cpi->nmvcosts[0],
                  vpx_calloc(MV_VALS, sizeof(*cpi->nmvcosts[0])));
  CHECK_MEM_ERROR(cm, cpi->nmvcosts[1],
                  vpx_calloc(MV_VALS, sizeof(*cpi->nmvcosts[1])));
  CHECK_MEM_ERROR(cm, cpi->nmvcosts_hp[0],
                  vpx_calloc(MV_VALS, sizeof(*cpi->nmvcosts_hp[0])));
  CHECK_MEM_ERROR(cm, cpi->nmvcosts_hp[1],
                  vpx_calloc(MV_VALS, sizeof(*cpi->nmvcosts_hp[1])));
  CHECK_MEM_ERROR(cm, cpi->nmvsadcosts[0],
                  vpx_calloc(MV_VALS, sizeof(*cpi->nmvsadcosts[0])));
  CHECK_MEM_ERROR(cm, cpi->nmvsadcosts[1],
                  vpx_calloc(MV_VALS, sizeof(*cpi->nmvsadcosts[1])));
  CHECK_MEM_ERROR(cm, cpi->nmvsadcosts_hp[0],
                  vpx_calloc(MV_VALS, sizeof(*cpi->nmvsadcosts_hp[0])));
  CHECK_MEM_ERROR(cm, cpi->nmvsadcosts_hp[1],
                  vpx_calloc(MV_VALS, sizeof(*cpi->nmvsadcosts_hp[1])));

  for (i = 0; i < (sizeof(cpi->mbgraph_stats) /
                   sizeof(cpi->mbgraph_stats[0])); i++) {
    CHECK_MEM_ERROR(cm, cpi->mbgraph_stats[i].mb_stats,
                    vpx_calloc(cm->MBs *
                               sizeof(*cpi->mbgraph_stats[i].mb_stats), 1));
  }

#if CONFIG_FP_MB_STATS
  cpi->use_fp_mb_stats = 0;
  if (cpi->use_fp_mb_stats) {
    // a place holder used to store the first pass mb stats in the first pass
    CHECK_MEM_ERROR(cm, cpi->twopass.frame_mb_stats_buf,
                    vpx_calloc(cm->MBs * sizeof(uint8_t), 1));
  } else {
    cpi->twopass.frame_mb_stats_buf = NULL;
  }
#endif

  cpi->refresh_alt_ref_frame = 0;

  cpi->b_calculate_psnr = CONFIG_INTERNAL_STATS;
#if CONFIG_INTERNAL_STATS
  cpi->b_calculate_ssimg = 0;

  cpi->count = 0;
  cpi->bytes = 0;

  if (cpi->b_calculate_psnr) {
    cpi->total_y = 0.0;
    cpi->total_u = 0.0;
    cpi->total_v = 0.0;
    cpi->total = 0.0;
    cpi->total_sq_error = 0;
    cpi->total_samples = 0;

    cpi->totalp_y = 0.0;
    cpi->totalp_u = 0.0;
    cpi->totalp_v = 0.0;
    cpi->totalp = 0.0;
    cpi->totalp_sq_error = 0;
    cpi->totalp_samples = 0;

    cpi->tot_recode_hits = 0;
    cpi->summed_quality = 0;
    cpi->summed_weights = 0;
    cpi->summedp_quality = 0;
    cpi->summedp_weights = 0;
  }

  if (cpi->b_calculate_ssimg) {
    cpi->total_ssimg_y = 0;
    cpi->total_ssimg_u = 0;
    cpi->total_ssimg_v = 0;
    cpi->total_ssimg_all = 0;
  }

#endif

  cpi->first_time_stamp_ever = INT64_MAX;

  cal_nmvjointsadcost(cpi->mb.nmvjointsadcost);
#if CONFIG_INTRABC
  cpi->mb.ndvcost[0] = &cpi->ndvcosts[0][MV_MAX];
  cpi->mb.ndvcost[1] = &cpi->ndvcosts[1][MV_MAX];
#endif  // CONFIG_INTRABC
  cpi->mb.nmvcost[0] = &cpi->nmvcosts[0][MV_MAX];
  cpi->mb.nmvcost[1] = &cpi->nmvcosts[1][MV_MAX];
  cpi->mb.nmvsadcost[0] = &cpi->nmvsadcosts[0][MV_MAX];
  cpi->mb.nmvsadcost[1] = &cpi->nmvsadcosts[1][MV_MAX];
  cal_nmvsadcosts(cpi->mb.nmvsadcost);

  cpi->mb.nmvcost_hp[0] = &cpi->nmvcosts_hp[0][MV_MAX];
  cpi->mb.nmvcost_hp[1] = &cpi->nmvcosts_hp[1][MV_MAX];
  cpi->mb.nmvsadcost_hp[0] = &cpi->nmvsadcosts_hp[0][MV_MAX];
  cpi->mb.nmvsadcost_hp[1] = &cpi->nmvsadcosts_hp[1][MV_MAX];
  cal_nmvsadcosts_hp(cpi->mb.nmvsadcost_hp);

#if CONFIG_VP9_TEMPORAL_DENOISING
#ifdef OUTPUT_YUV_DENOISED
  yuv_denoised_file = fopen("denoised.yuv", "ab");
#endif
#endif
#ifdef OUTPUT_YUV_REC
  yuv_rec_file = fopen("rec.yuv", "wb");
#endif

#if 0
  framepsnr = fopen("framepsnr.stt", "a");
  kf_list = fopen("kf_list.stt", "w");
#endif

  cpi->allow_encode_breakout = ENCODE_BREAKOUT_ENABLED;

  if (oxcf->pass == 1) {
    vp9_init_first_pass(cpi);
  } else if (oxcf->pass == 2) {
    const size_t packet_sz = sizeof(FIRSTPASS_STATS);
    const int packets = (int)(oxcf->two_pass_stats_in.sz / packet_sz);

#if CONFIG_FP_MB_STATS
    if (cpi->use_fp_mb_stats) {
      const size_t psz = cpi->common.MBs * sizeof(uint8_t);
      const int ps = (int)(oxcf->firstpass_mb_stats_in.sz / psz);

      cpi->twopass.firstpass_mb_stats.mb_stats_start =
          oxcf->firstpass_mb_stats_in.buf;
      cpi->twopass.firstpass_mb_stats.mb_stats_end =
          cpi->twopass.firstpass_mb_stats.mb_stats_start +
          (ps - 1) * cpi->common.MBs * sizeof(uint8_t);
    }
#endif

    cpi->twopass.stats_in_start = oxcf->two_pass_stats_in.buf;
    cpi->twopass.stats_in = cpi->twopass.stats_in_start;
    cpi->twopass.stats_in_end = &cpi->twopass.stats_in[packets - 1];

    vp9_init_second_pass(cpi);
  }

  vp9_set_speed_features(cpi);

  // Allocate memory to store variances for a frame.
  CHECK_MEM_ERROR(cm, cpi->source_diff_var,
                  vpx_calloc(cm->MBs, sizeof(diff)));
  cpi->source_var_thresh = 0;
  cpi->frames_till_next_var_check = 0;

  // Default rd threshold factors for mode selection
  for (i = 0; i < BLOCK_SIZES; ++i) {
    for (j = 0; j < MAX_MODES; ++j) {
      cpi->rd.thresh_freq_fact[i][j] = 32;
      cpi->rd.mode_map[i][j] = j;
    }
  }

#define BFP(BT, SDF, SDAF, VF, SVF, SVAF, SDX3F, SDX8F, SDX4DF)\
    cpi->fn_ptr[BT].sdf            = SDF; \
    cpi->fn_ptr[BT].sdaf           = SDAF; \
    cpi->fn_ptr[BT].vf             = VF; \
    cpi->fn_ptr[BT].svf            = SVF; \
    cpi->fn_ptr[BT].svaf           = SVAF; \
    cpi->fn_ptr[BT].sdx3f          = SDX3F; \
    cpi->fn_ptr[BT].sdx8f          = SDX8F; \
    cpi->fn_ptr[BT].sdx4df         = SDX4DF;

#if CONFIG_EXT_CODING_UNIT_SIZE
  BFP(BLOCK_128X128, vp9_sad128x128, vp9_sad128x128_avg,
      vp9_variance128x128, vp9_sub_pixel_variance128x128,
      vp9_sub_pixel_avg_variance128x128, vp9_sad128x128x3, vp9_sad128x128x8,
      vp9_sad128x128x4d)

  BFP(BLOCK_128X64, vp9_sad128x64, vp9_sad128x64_avg,
      vp9_variance128x64, vp9_sub_pixel_variance128x64,
      vp9_sub_pixel_avg_variance128x64, NULL, NULL, vp9_sad128x64x4d)

  BFP(BLOCK_64X128, vp9_sad64x128, vp9_sad64x128_avg,
      vp9_variance64x128, vp9_sub_pixel_variance64x128,
      vp9_sub_pixel_avg_variance64x128, NULL, NULL, vp9_sad64x128x4d)
#endif

  BFP(BLOCK_32X16, vp9_sad32x16, vp9_sad32x16_avg,
      vp9_variance32x16, vp9_sub_pixel_variance32x16,
      vp9_sub_pixel_avg_variance32x16, NULL, NULL, vp9_sad32x16x4d)

  BFP(BLOCK_16X32, vp9_sad16x32, vp9_sad16x32_avg,
      vp9_variance16x32, vp9_sub_pixel_variance16x32,
      vp9_sub_pixel_avg_variance16x32, NULL, NULL, vp9_sad16x32x4d)

  BFP(BLOCK_64X32, vp9_sad64x32, vp9_sad64x32_avg,
      vp9_variance64x32, vp9_sub_pixel_variance64x32,
      vp9_sub_pixel_avg_variance64x32, NULL, NULL, vp9_sad64x32x4d)

  BFP(BLOCK_32X64, vp9_sad32x64, vp9_sad32x64_avg,
      vp9_variance32x64, vp9_sub_pixel_variance32x64,
      vp9_sub_pixel_avg_variance32x64, NULL, NULL, vp9_sad32x64x4d)

  BFP(BLOCK_32X32, vp9_sad32x32, vp9_sad32x32_avg,
      vp9_variance32x32, vp9_sub_pixel_variance32x32,
      vp9_sub_pixel_avg_variance32x32, vp9_sad32x32x3, vp9_sad32x32x8,
      vp9_sad32x32x4d)

  BFP(BLOCK_64X64, vp9_sad64x64, vp9_sad64x64_avg,
      vp9_variance64x64, vp9_sub_pixel_variance64x64,
      vp9_sub_pixel_avg_variance64x64, vp9_sad64x64x3, vp9_sad64x64x8,
      vp9_sad64x64x4d)

  BFP(BLOCK_16X16, vp9_sad16x16, vp9_sad16x16_avg,
      vp9_variance16x16, vp9_sub_pixel_variance16x16,
      vp9_sub_pixel_avg_variance16x16, vp9_sad16x16x3, vp9_sad16x16x8,
      vp9_sad16x16x4d)

  BFP(BLOCK_16X8, vp9_sad16x8, vp9_sad16x8_avg,
      vp9_variance16x8, vp9_sub_pixel_variance16x8,
      vp9_sub_pixel_avg_variance16x8,
      vp9_sad16x8x3, vp9_sad16x8x8, vp9_sad16x8x4d)

  BFP(BLOCK_8X16, vp9_sad8x16, vp9_sad8x16_avg,
      vp9_variance8x16, vp9_sub_pixel_variance8x16,
      vp9_sub_pixel_avg_variance8x16,
      vp9_sad8x16x3, vp9_sad8x16x8, vp9_sad8x16x4d)

  BFP(BLOCK_8X8, vp9_sad8x8, vp9_sad8x8_avg,
      vp9_variance8x8, vp9_sub_pixel_variance8x8,
      vp9_sub_pixel_avg_variance8x8,
      vp9_sad8x8x3, vp9_sad8x8x8, vp9_sad8x8x4d)

  BFP(BLOCK_8X4, vp9_sad8x4, vp9_sad8x4_avg,
      vp9_variance8x4, vp9_sub_pixel_variance8x4,
      vp9_sub_pixel_avg_variance8x4, NULL, vp9_sad8x4x8, vp9_sad8x4x4d)

  BFP(BLOCK_4X8, vp9_sad4x8, vp9_sad4x8_avg,
      vp9_variance4x8, vp9_sub_pixel_variance4x8,
      vp9_sub_pixel_avg_variance4x8, NULL, vp9_sad4x8x8, vp9_sad4x8x4d)

  BFP(BLOCK_4X4, vp9_sad4x4, vp9_sad4x4_avg,
      vp9_variance4x4, vp9_sub_pixel_variance4x4,
      vp9_sub_pixel_avg_variance4x4,
      vp9_sad4x4x3, vp9_sad4x4x8, vp9_sad4x4x4d)

#if CONFIG_WEDGE_PARTITION
#define MBFP(BT, MSDF, MVF, MSVF)         \
  cpi->fn_ptr[BT].msdf            = MSDF; \
  cpi->fn_ptr[BT].mvf             = MVF;  \
  cpi->fn_ptr[BT].msvf            = MSVF;

#if CONFIG_EXT_CODING_UNIT_SIZE
  MBFP(BLOCK_128X128, vp9_masked_sad128x128, vp9_masked_variance128x128,
       vp9_masked_sub_pixel_variance128x128)
  MBFP(BLOCK_128X64, vp9_masked_sad128x64, vp9_masked_variance128x64,
         vp9_masked_sub_pixel_variance128x64)
  MBFP(BLOCK_64X128, vp9_masked_sad64x128, vp9_masked_variance64x128,
         vp9_masked_sub_pixel_variance64x128)
#endif
  MBFP(BLOCK_64X64, vp9_masked_sad64x64, vp9_masked_variance64x64,
       vp9_masked_sub_pixel_variance64x64)
  MBFP(BLOCK_64X32, vp9_masked_sad64x32, vp9_masked_variance64x32,
         vp9_masked_sub_pixel_variance64x32)
  MBFP(BLOCK_32X64, vp9_masked_sad32x64, vp9_masked_variance32x64,
         vp9_masked_sub_pixel_variance32x64)
  MBFP(BLOCK_32X32, vp9_masked_sad32x32, vp9_masked_variance32x32,
       vp9_masked_sub_pixel_variance32x32)
  MBFP(BLOCK_32X16, vp9_masked_sad32x16, vp9_masked_variance32x16,
       vp9_masked_sub_pixel_variance32x16)
  MBFP(BLOCK_16X32, vp9_masked_sad16x32, vp9_masked_variance16x32,
       vp9_masked_sub_pixel_variance16x32)
  MBFP(BLOCK_16X16, vp9_masked_sad16x16, vp9_masked_variance16x16,
         vp9_masked_sub_pixel_variance16x16)
  MBFP(BLOCK_16X8, vp9_masked_sad16x8, vp9_masked_variance16x8,
         vp9_masked_sub_pixel_variance16x8)
  MBFP(BLOCK_8X16, vp9_masked_sad8x16, vp9_masked_variance8x16,
         vp9_masked_sub_pixel_variance8x16)
  MBFP(BLOCK_8X8, vp9_masked_sad8x8, vp9_masked_variance8x8,
       vp9_masked_sub_pixel_variance8x8)
  MBFP(BLOCK_4X8, vp9_masked_sad4x8, vp9_masked_variance4x8,
       vp9_masked_sub_pixel_variance4x8)
  MBFP(BLOCK_8X4, vp9_masked_sad8x4, vp9_masked_variance8x4,
       vp9_masked_sub_pixel_variance8x4)
  MBFP(BLOCK_4X4, vp9_masked_sad4x4, vp9_masked_variance4x4,
       vp9_masked_sub_pixel_variance4x4)
#endif  // CONFIG_WEDGE_PARTITION

#if CONFIG_VP9_HIGHBITDEPTH
  highbd_set_var_fns(cpi);
#endif

  /* vp9_init_quantizer() is first called here. Add check in
   * vp9_frame_init_quantizer() so that vp9_init_quantizer is only
   * called later when needed. This will avoid unnecessary calls of
   * vp9_init_quantizer() for every frame.
   */
  vp9_init_quantizer(cpi);

  vp9_loop_filter_init(cm);

  cm->error.setjmp = 0;

  return cpi;
}

void vp9_remove_compressor(VP9_COMP *cpi) {
  unsigned int i;

  if (!cpi)
    return;

  if (cpi && (cpi->common.current_video_frame > 0)) {
#if CONFIG_INTERNAL_STATS

    vp9_clear_system_state();

    // printf("\n8x8-4x4:%d-%d\n", cpi->t8x8_count, cpi->t4x4_count);
    if (cpi->oxcf.pass != 1) {
      FILE *f = fopen("opsnr.stt", "a");
      double time_encoded = (cpi->last_end_time_stamp_seen
                             - cpi->first_time_stamp_ever) / 10000000.000;
      double total_encode_time = (cpi->time_receive_data +
                                  cpi->time_compress_data)   / 1000.000;
      const double dr =
          (double)cpi->bytes * (double) 8 / (double)1000 / time_encoded;
      const double peak = (double)((1 << cpi->oxcf.input_bit_depth) - 1);

      if (cpi->b_calculate_psnr) {
        const double total_psnr =
            vpx_sse_to_psnr((double)cpi->total_samples, peak,
                            (double)cpi->total_sq_error);
        const double totalp_psnr =
            vpx_sse_to_psnr((double)cpi->totalp_samples, peak,
                            (double)cpi->totalp_sq_error);
        const double total_ssim = 100 * pow(cpi->summed_quality /
                                                cpi->summed_weights, 8.0);
        const double totalp_ssim = 100 * pow(cpi->summedp_quality /
                                                cpi->summedp_weights, 8.0);

        fprintf(f, "Bitrate\tAVGPsnr\tGLBPsnr\tAVPsnrP\tGLPsnrP\t"
                "VPXSSIM\tVPSSIMP\t  Time(ms)\n");
        fprintf(f, "%7.2f\t%7.3f\t%7.3f\t%7.3f\t%7.3f\t%7.3f\t%7.3f\t%8.0f\n",
                dr, cpi->total / cpi->count, total_psnr,
                cpi->totalp / cpi->count, totalp_psnr, total_ssim, totalp_ssim,
                total_encode_time);
      }

      if (cpi->b_calculate_ssimg) {
        fprintf(f, "BitRate\tSSIM_Y\tSSIM_U\tSSIM_V\tSSIM_A\t  Time(ms)\n");
        fprintf(f, "%7.2f\t%6.4f\t%6.4f\t%6.4f\t%6.4f\t%8.0f\n", dr,
                cpi->total_ssimg_y / cpi->count,
                cpi->total_ssimg_u / cpi->count,
                cpi->total_ssimg_v / cpi->count,
                cpi->total_ssimg_all / cpi->count, total_encode_time);
      }

      fclose(f);
    }

#endif

#if 0
    {
      printf("\n_pick_loop_filter_level:%d\n", cpi->time_pick_lpf / 1000);
      printf("\n_frames recive_data encod_mb_row compress_frame  Total\n");
      printf("%6d %10ld %10ld %10ld %10ld\n", cpi->common.current_video_frame,
             cpi->time_receive_data / 1000, cpi->time_encode_sb_row / 1000,
             cpi->time_compress_data / 1000,
             (cpi->time_receive_data + cpi->time_compress_data) / 1000);
    }
#endif
  }

#if CONFIG_VP9_TEMPORAL_DENOISING
  if (cpi->oxcf.noise_sensitivity > 0) {
    vp9_denoiser_free(&(cpi->denoiser));
  }
#endif

  dealloc_compressor_data(cpi);
  vpx_free(cpi->tok);

  for (i = 0; i < sizeof(cpi->mbgraph_stats) /
                  sizeof(cpi->mbgraph_stats[0]); ++i) {
    vpx_free(cpi->mbgraph_stats[i].mb_stats);
  }

#if CONFIG_FP_MB_STATS
  if (cpi->use_fp_mb_stats) {
    vpx_free(cpi->twopass.frame_mb_stats_buf);
    cpi->twopass.frame_mb_stats_buf = NULL;
  }
#endif

  vp9_remove_common(&cpi->common);
  vpx_free(cpi);

#if CONFIG_VP9_TEMPORAL_DENOISING
#ifdef OUTPUT_YUV_DENOISED
  fclose(yuv_denoised_file);
#endif
#endif
#ifdef OUTPUT_YUV_REC
  fclose(yuv_rec_file);
#endif

#if 0

  if (keyfile)
    fclose(keyfile);

  if (framepsnr)
    fclose(framepsnr);

  if (kf_list)
    fclose(kf_list);

#endif
}

static int64_t get_sse(const uint8_t *a, int a_stride,
                       const uint8_t *b, int b_stride,
                       int width, int height) {
  const int dw = width % 16;
  const int dh = height % 16;
  int64_t total_sse = 0;
  unsigned int sse = 0;
  int sum = 0;
  int x, y;

  if (dw > 0) {
    variance(&a[width - dw], a_stride, &b[width - dw], b_stride,
             dw, height, &sse, &sum);
    total_sse += sse;
  }

  if (dh > 0) {
    variance(&a[(height - dh) * a_stride], a_stride,
             &b[(height - dh) * b_stride], b_stride,
             width - dw, dh, &sse, &sum);
    total_sse += sse;
  }

  for (y = 0; y < height / 16; ++y) {
    const uint8_t *pa = a;
    const uint8_t *pb = b;
    for (x = 0; x < width / 16; ++x) {
      vp9_mse16x16(pa, a_stride, pb, b_stride, &sse);
      total_sse += sse;

      pa += 16;
      pb += 16;
    }

    a += 16 * a_stride;
    b += 16 * b_stride;
  }

  return total_sse;
}

#if CONFIG_VP9_HIGHBITDEPTH
static int64_t highbd_get_sse_shift(const uint8_t *a8, int a_stride,
                                    const uint8_t *b8, int b_stride,
                                    int width, int height,
                                    unsigned int input_shift) {
  const uint16_t *a = CONVERT_TO_SHORTPTR(a8);
  const uint16_t *b = CONVERT_TO_SHORTPTR(b8);
  int64_t total_sse = 0;
  int x, y;
  for (y = 0; y < height; ++y) {
    for (x = 0; x < width; ++x) {
      int64_t diff;
      diff = (a[x] >> input_shift) - (b[x] >> input_shift);
      total_sse += diff * diff;
    }
    a += a_stride;
    b += b_stride;
  }
  return total_sse;
}

static int64_t highbd_get_sse(const uint8_t *a, int a_stride,
                              const uint8_t *b, int b_stride,
                              int width, int height) {
  int64_t total_sse = 0;
  int x, y;
  const int dw = width % 16;
  const int dh = height % 16;
  unsigned int sse = 0;
  int sum = 0;
  if (dw > 0) {
    highbd_variance(&a[width - dw], a_stride, &b[width - dw], b_stride,
                    dw, height, &sse, &sum);
    total_sse += sse;
  }
  if (dh > 0) {
    highbd_variance(&a[(height - dh) * a_stride], a_stride,
                    &b[(height - dh) * b_stride], b_stride,
                    width - dw, dh, &sse, &sum);
    total_sse += sse;
  }
  for (y = 0; y < height / 16; ++y) {
    const uint8_t *pa = a;
    const uint8_t *pb = b;
    for (x = 0; x < width / 16; ++x) {
      vp9_highbd_mse16x16(pa, a_stride, pb, b_stride, &sse);
      total_sse += sse;
      pa += 16;
      pb += 16;
    }
    a += 16 * a_stride;
    b += 16 * b_stride;
  }
  return total_sse;
}
#endif  // CONFIG_VP9_HIGHBITDEPTH

typedef struct {
  double psnr[4];       // total/y/u/v
  uint64_t sse[4];      // total/y/u/v
  uint32_t samples[4];  // total/y/u/v
} PSNR_STATS;

static void calc_psnr(const YV12_BUFFER_CONFIG *a, const YV12_BUFFER_CONFIG *b,
                      PSNR_STATS *psnr) {
  static const double peak = 255.0;
  const int widths[3]        = {a->y_width,  a->uv_width,  a->uv_width };
  const int heights[3]       = {a->y_height, a->uv_height, a->uv_height};
  const uint8_t *a_planes[3] = {a->y_buffer, a->u_buffer,  a->v_buffer };
  const int a_strides[3]     = {a->y_stride, a->uv_stride, a->uv_stride};
  const uint8_t *b_planes[3] = {b->y_buffer, b->u_buffer,  b->v_buffer };
  const int b_strides[3]     = {b->y_stride, b->uv_stride, b->uv_stride};
  int i;
  uint64_t total_sse = 0;
  uint32_t total_samples = 0;

  for (i = 0; i < 3; ++i) {
    const int w = widths[i];
    const int h = heights[i];
    const uint32_t samples = w * h;
    const uint64_t sse = get_sse(a_planes[i], a_strides[i],
                                 b_planes[i], b_strides[i],
                                 w, h);
    psnr->sse[1 + i] = sse;
    psnr->samples[1 + i] = samples;
    psnr->psnr[1 + i] = vpx_sse_to_psnr(samples, peak, (double)sse);

    total_sse += sse;
    total_samples += samples;
  }

  psnr->sse[0] = total_sse;
  psnr->samples[0] = total_samples;
  psnr->psnr[0] = vpx_sse_to_psnr((double)total_samples, peak,
                                  (double)total_sse);
}

#if CONFIG_VP9_HIGHBITDEPTH
static void calc_highbd_psnr(const YV12_BUFFER_CONFIG *a,
                             const YV12_BUFFER_CONFIG *b,
                             PSNR_STATS *psnr,
                             unsigned int bit_depth,
                             unsigned int in_bit_depth) {
  const int widths[3] = {a->y_width,  a->uv_width,  a->uv_width };
  const int heights[3] = {a->y_height, a->uv_height, a->uv_height};
  const uint8_t *a_planes[3] = {a->y_buffer, a->u_buffer,  a->v_buffer };
  const int a_strides[3] = {a->y_stride, a->uv_stride, a->uv_stride};
  const uint8_t *b_planes[3] = {b->y_buffer, b->u_buffer,  b->v_buffer };
  const int b_strides[3] = {b->y_stride, b->uv_stride, b->uv_stride};
  int i;
  uint64_t total_sse = 0;
  uint32_t total_samples = 0;
  const double peak = (double)((1 << in_bit_depth) - 1);
  const unsigned int input_shift = bit_depth - in_bit_depth;

  for (i = 0; i < 3; ++i) {
    const int w = widths[i];
    const int h = heights[i];
    const uint32_t samples = w * h;
    uint64_t sse;
    if (a->flags & YV12_FLAG_HIGHBITDEPTH) {
      if (input_shift) {
        sse = highbd_get_sse_shift(a_planes[i], a_strides[i],
                                   b_planes[i], b_strides[i], w, h,
                                   input_shift);
      } else {
        sse = highbd_get_sse(a_planes[i], a_strides[i],
                             b_planes[i], b_strides[i], w, h);
      }
    } else {
      sse = get_sse(a_planes[i], a_strides[i],
                    b_planes[i], b_strides[i],
                    w, h);
    }
    psnr->sse[1 + i] = sse;
    psnr->samples[1 + i] = samples;
    psnr->psnr[1 + i] = vpx_sse_to_psnr(samples, peak, (double)sse);

    total_sse += sse;
    total_samples += samples;
  }

  psnr->sse[0] = total_sse;
  psnr->samples[0] = total_samples;
  psnr->psnr[0] = vpx_sse_to_psnr((double)total_samples, peak,
                                  (double)total_sse);
}
#endif  // CONFIG_VP9_HIGHBITDEPTH

static void generate_psnr_packet(VP9_COMP *cpi) {
  struct vpx_codec_cx_pkt pkt;
  int i;
  PSNR_STATS psnr;
#if CONFIG_VP9_HIGHBITDEPTH
  calc_highbd_psnr(cpi->Source, cpi->common.frame_to_show, &psnr,
                   cpi->mb.e_mbd.bd, cpi->oxcf.input_bit_depth);
#else
  calc_psnr(cpi->Source, cpi->common.frame_to_show, &psnr);
#endif

  for (i = 0; i < 4; ++i) {
    pkt.data.psnr.samples[i] = psnr.samples[i];
    pkt.data.psnr.sse[i] = psnr.sse[i];
    pkt.data.psnr.psnr[i] = psnr.psnr[i];
  }
  pkt.kind = VPX_CODEC_PSNR_PKT;
  vpx_codec_pkt_list_add(cpi->output_pkt_list, &pkt);
}

int vp9_use_as_reference(VP9_COMP *cpi, int ref_frame_flags) {
  if (ref_frame_flags > ((1 << REFS_PER_FRAME) - 1))
    return -1;

  cpi->ref_frame_flags = ref_frame_flags;
  return 0;
}

void vp9_update_reference(VP9_COMP *cpi, int ref_frame_flags) {
  cpi->ext_refresh_golden_frame = (ref_frame_flags & VP9_GOLD_FLAG) != 0;
  cpi->ext_refresh_alt_ref_frame = (ref_frame_flags & VP9_ALT_FLAG) != 0;
  cpi->ext_refresh_last_frame = (ref_frame_flags & VP9_LAST_FLAG) != 0;
#if CONFIG_MULTI_REF
  cpi->ext_refresh_last2_frame = (ref_frame_flags & VP9_LAST2_FLAG) != 0;
  cpi->ext_refresh_last3_frame = (ref_frame_flags & VP9_LAST3_FLAG) != 0;
  cpi->ext_refresh_last4_frame = (ref_frame_flags & VP9_LAST4_FLAG) != 0;
#endif  // CONFIG_MULTI_REF
  cpi->ext_refresh_frame_flags_pending = 1;
}

static YV12_BUFFER_CONFIG *get_vp9_ref_frame_buffer(VP9_COMP *cpi,
                                VP9_REFFRAME ref_frame_flag) {
  MV_REFERENCE_FRAME ref_frame = NONE;
  if (ref_frame_flag == VP9_LAST_FLAG)
    ref_frame = LAST_FRAME;
#if CONFIG_MULTI_REF
  else if (ref_frame_flag == VP9_LAST2_FLAG)
    ref_frame = LAST2_FRAME;
  else if (ref_frame_flag == VP9_LAST3_FLAG)
    ref_frame = LAST3_FRAME;
  else if (ref_frame_flag == VP9_LAST4_FLAG)
    ref_frame = LAST4_FRAME;
#endif  // CONFIG_MULTI_REF
  else if (ref_frame_flag == VP9_GOLD_FLAG)
    ref_frame = GOLDEN_FRAME;
  else if (ref_frame_flag == VP9_ALT_FLAG)
    ref_frame = ALTREF_FRAME;

  return ref_frame == NONE ? NULL : get_ref_frame_buffer(cpi, ref_frame);
}

int vp9_copy_reference_enc(VP9_COMP *cpi, VP9_REFFRAME ref_frame_flag,
                           YV12_BUFFER_CONFIG *sd) {
  YV12_BUFFER_CONFIG *cfg = get_vp9_ref_frame_buffer(cpi, ref_frame_flag);
  if (cfg) {
    vp8_yv12_copy_frame(cfg, sd);
    return 0;
  } else {
    return -1;
  }
}

int vp9_set_reference_enc(VP9_COMP *cpi, VP9_REFFRAME ref_frame_flag,
                          YV12_BUFFER_CONFIG *sd) {
  YV12_BUFFER_CONFIG *cfg = get_vp9_ref_frame_buffer(cpi, ref_frame_flag);
  if (cfg) {
    vp8_yv12_copy_frame(sd, cfg);
    return 0;
  } else {
    return -1;
  }
}

int vp9_update_entropy(VP9_COMP * cpi, int update) {
  cpi->ext_refresh_frame_context = update;
  cpi->ext_refresh_frame_context_pending = 1;
  return 0;
}

#if CONFIG_VP9_TEMPORAL_DENOISING
#if defined(OUTPUT_YUV_DENOISED)
// The denoiser buffer is allocated as a YUV 440 buffer. This function writes it
// as YUV 420. We simply use the top-left pixels of the UV buffers, since we do
// not denoise the UV channels at this time. If ever we implement UV channel
// denoising we will have to modify this.
void vp9_write_yuv_frame_420(YV12_BUFFER_CONFIG *s, FILE *f) {
  uint8_t *src = s->y_buffer;
  int h = s->y_height;

  do {
    fwrite(src, s->y_width, 1, f);
    src += s->y_stride;
  } while (--h);

  src = s->u_buffer;
  h = s->uv_height / 2;

  do {
    fwrite(src, s->uv_width / 2, 1, f);
    src += s->uv_stride + s->uv_width / 2;
  } while (--h);

  src = s->v_buffer;
  h = s->uv_height / 2;

  do {
    fwrite(src, s->uv_width / 2, 1, f);
    src += s->uv_stride + s->uv_width / 2;
  } while (--h);
}
#endif
#endif

#ifdef OUTPUT_YUV_REC
void vp9_write_yuv_rec_frame(VP9_COMMON *cm) {
  YV12_BUFFER_CONFIG *s = cm->frame_to_show;
  uint8_t *src = s->y_buffer;
  int h = cm->height;

#if CONFIG_VP9_HIGHBITDEPTH
  if (s->flags & YV12_FLAG_HIGHBITDEPTH) {
    uint16_t *src16 = CONVERT_TO_SHORTPTR(s->y_buffer);

    do {
      fwrite(src16, s->y_width, 2,  yuv_rec_file);
      src16 += s->y_stride;
    } while (--h);

    src16 = CONVERT_TO_SHORTPTR(s->u_buffer);
    h = s->uv_height;

    do {
      fwrite(src16, s->uv_width, 2,  yuv_rec_file);
      src16 += s->uv_stride;
    } while (--h);

    src16 = CONVERT_TO_SHORTPTR(s->v_buffer);
    h = s->uv_height;

    do {
      fwrite(src16, s->uv_width, 2, yuv_rec_file);
      src16 += s->uv_stride;
    } while (--h);

    fflush(yuv_rec_file);
    return;
  }
#endif  // CONFIG_VP9_HIGHBITDEPTH

  do {
    fwrite(src, s->y_width, 1,  yuv_rec_file);
    src += s->y_stride;
  } while (--h);

  src = s->u_buffer;
  h = s->uv_height;

  do {
    fwrite(src, s->uv_width, 1,  yuv_rec_file);
    src += s->uv_stride;
  } while (--h);

  src = s->v_buffer;
  h = s->uv_height;

  do {
    fwrite(src, s->uv_width, 1, yuv_rec_file);
    src += s->uv_stride;
  } while (--h);

  fflush(yuv_rec_file);
}
#endif

#if CONFIG_VP9_HIGHBITDEPTH
static void scale_and_extend_frame_nonnormative(const YV12_BUFFER_CONFIG *src,
                                                YV12_BUFFER_CONFIG *dst,
                                                int bd) {
#else
static void scale_and_extend_frame_nonnormative(const YV12_BUFFER_CONFIG *src,
                                                YV12_BUFFER_CONFIG *dst) {
#endif  // CONFIG_VP9_HIGHBITDEPTH
  // TODO(dkovalev): replace YV12_BUFFER_CONFIG with vpx_image_t
  int i;
  const uint8_t *const srcs[3] = {src->y_buffer, src->u_buffer, src->v_buffer};
  const int src_strides[3] = {src->y_stride, src->uv_stride, src->uv_stride};
  const int src_widths[3] = {src->y_crop_width, src->uv_crop_width,
                             src->uv_crop_width };
  const int src_heights[3] = {src->y_crop_height, src->uv_crop_height,
                              src->uv_crop_height};
  uint8_t *const dsts[3] = {dst->y_buffer, dst->u_buffer, dst->v_buffer};
  const int dst_strides[3] = {dst->y_stride, dst->uv_stride, dst->uv_stride};
  const int dst_widths[3] = {dst->y_crop_width, dst->uv_crop_width,
                             dst->uv_crop_width};
  const int dst_heights[3] = {dst->y_crop_height, dst->uv_crop_height,
                              dst->uv_crop_height};

  for (i = 0; i < MAX_MB_PLANE; ++i) {
#if CONFIG_VP9_HIGHBITDEPTH
    if (src->flags & YV12_FLAG_HIGHBITDEPTH) {
      vp9_highbd_resize_plane(srcs[i], src_heights[i], src_widths[i],
                              src_strides[i], dsts[i], dst_heights[i],
                              dst_widths[i], dst_strides[i], bd);
    } else {
      vp9_resize_plane(srcs[i], src_heights[i], src_widths[i], src_strides[i],
                       dsts[i], dst_heights[i], dst_widths[i], dst_strides[i]);
    }
#else
    vp9_resize_plane(srcs[i], src_heights[i], src_widths[i], src_strides[i],
                     dsts[i], dst_heights[i], dst_widths[i], dst_strides[i]);
#endif  // CONFIG_VP9_HIGHBITDEPTH
  }
  vp9_extend_frame_borders(dst);
}

#if CONFIG_VP9_HIGHBITDEPTH
static void scale_and_extend_frame(const YV12_BUFFER_CONFIG *src,
                                   YV12_BUFFER_CONFIG *dst, int bd) {
#else
static void scale_and_extend_frame(const YV12_BUFFER_CONFIG *src,
                                   YV12_BUFFER_CONFIG *dst) {
#endif  // CONFIG_VP9_HIGHBITDEPTH
  const int src_w = src->y_crop_width;
  const int src_h = src->y_crop_height;
  const int dst_w = dst->y_crop_width;
  const int dst_h = dst->y_crop_height;
  const uint8_t *const srcs[3] = {src->y_buffer, src->u_buffer, src->v_buffer};
  const int src_strides[3] = {src->y_stride, src->uv_stride, src->uv_stride};
  uint8_t *const dsts[3] = {dst->y_buffer, dst->u_buffer, dst->v_buffer};
  const int dst_strides[3] = {dst->y_stride, dst->uv_stride, dst->uv_stride};
  const InterpKernel *const kernel = vp9_get_interp_kernel(EIGHTTAP);
  int x, y, i;

  for (y = 0; y < dst_h; y += 16) {
    for (x = 0; x < dst_w; x += 16) {
      for (i = 0; i < MAX_MB_PLANE; ++i) {
        const int factor = (i == 0 || i == 3 ? 1 : 2);
        const int x_q4 = x * (16 / factor) * src_w / dst_w;
        const int y_q4 = y * (16 / factor) * src_h / dst_h;
        const int src_stride = src_strides[i];
        const int dst_stride = dst_strides[i];
        const uint8_t *src_ptr = srcs[i] + (y / factor) * src_h / dst_h *
                                     src_stride + (x / factor) * src_w / dst_w;
        uint8_t *dst_ptr = dsts[i] + (y / factor) * dst_stride + (x / factor);

#if CONFIG_VP9_HIGHBITDEPTH
        if (src->flags & YV12_FLAG_HIGHBITDEPTH) {
          vp9_highbd_convolve8(src_ptr, src_stride, dst_ptr, dst_stride,
                               kernel[x_q4 & 0xf], 16 * src_w / dst_w,
                               kernel[y_q4 & 0xf], 16 * src_h / dst_h,
                               16 / factor, 16 / factor, bd);
        } else {
          vp9_convolve8(src_ptr, src_stride, dst_ptr, dst_stride,
                        kernel[x_q4 & 0xf], 16 * src_w / dst_w,
                        kernel[y_q4 & 0xf], 16 * src_h / dst_h,
                        16 / factor, 16 / factor);
        }
#else
        vp9_convolve8(src_ptr, src_stride, dst_ptr, dst_stride,
                      kernel[x_q4 & 0xf], 16 * src_w / dst_w,
                      kernel[y_q4 & 0xf], 16 * src_h / dst_h,
                      16 / factor, 16 / factor);
#endif  // CONFIG_VP9_HIGHBITDEPTH
      }
    }
  }

  vp9_extend_frame_borders(dst);
}

// Function to test for conditions that indicate we should loop
// back and recode a frame.
static int recode_loop_test(const VP9_COMP *cpi,
                            int high_limit, int low_limit,
                            int q, int maxq, int minq) {
  const VP9_COMMON *const cm = &cpi->common;
  const RATE_CONTROL *const rc = &cpi->rc;
  const VP9EncoderConfig *const oxcf = &cpi->oxcf;
  int force_recode = 0;

  // Special case trap if maximum allowed frame size exceeded.
  if (rc->projected_frame_size > rc->max_frame_bandwidth) {
    force_recode = 1;

  // Is frame recode allowed.
  // Yes if either recode mode 1 is selected or mode 2 is selected
  // and the frame is a key frame, golden frame or alt_ref_frame
  } else if ((cpi->sf.recode_loop == ALLOW_RECODE) ||
             ((cpi->sf.recode_loop == ALLOW_RECODE_KFARFGF) &&
              (cm->frame_type == KEY_FRAME ||
               cpi->refresh_golden_frame || cpi->refresh_alt_ref_frame))) {
    // General over and under shoot tests
    if ((rc->projected_frame_size > high_limit && q < maxq) ||
        (rc->projected_frame_size < low_limit && q > minq)) {
      force_recode = 1;
    } else if (cpi->oxcf.rc_mode == VPX_CQ) {
      // Deal with frame undershoot and whether or not we are
      // below the automatically set cq level.
      if (q > oxcf->cq_level &&
          rc->projected_frame_size < ((rc->this_frame_target * 7) >> 3)) {
        force_recode = 1;
      }
    }
  }
  return force_recode;
}

void vp9_update_reference_frames(VP9_COMP *cpi) {
  VP9_COMMON * const cm = &cpi->common;

  // At this point the new frame has been encoded.
  // If any buffer copy / swapping is signaled it should be done here.
  if (cm->frame_type == KEY_FRAME) {
    ref_cnt_fb(cm->frame_bufs,
               &cm->ref_frame_map[cpi->gld_fb_idx], cm->new_fb_idx);
    ref_cnt_fb(cm->frame_bufs,
               &cm->ref_frame_map[cpi->alt_fb_idx], cm->new_fb_idx);
  } else if (vp9_preserve_existing_gf(cpi)) {
    // We have decided to preserve the previously existing golden frame as our
    // new ARF frame. However, in the short term in function
    // vp9_bitstream.c::get_refresh_mask() we left it in the GF slot and, if
    // we're updating the GF with the current decoded frame, we save it to the
    // ARF slot instead.
    // We now have to update the ARF with the current frame and swap gld_fb_idx
    // and alt_fb_idx so that, overall, we've stored the old GF in the new ARF
    // slot and, if we're updating the GF, the current frame becomes the new GF.
    int tmp;

    ref_cnt_fb(cm->frame_bufs,
               &cm->ref_frame_map[cpi->alt_fb_idx], cm->new_fb_idx);

    tmp = cpi->alt_fb_idx;
    cpi->alt_fb_idx = cpi->gld_fb_idx;
    cpi->gld_fb_idx = tmp;
  } else { /* For non key/golden frames */
    if (cpi->refresh_alt_ref_frame) {
      int arf_idx = cpi->alt_fb_idx;
      if ((cpi->oxcf.pass == 2) && cpi->multi_arf_allowed) {
        const GF_GROUP *const gf_group = &cpi->twopass.gf_group;
        arf_idx = gf_group->arf_update_idx[gf_group->index];
      }

      ref_cnt_fb(cm->frame_bufs,
                 &cm->ref_frame_map[arf_idx], cm->new_fb_idx);
      vpx_memcpy(cpi->interp_filter_selected[ALTREF_FRAME],
                 cpi->interp_filter_selected[0],
                 sizeof(cpi->interp_filter_selected[0]));
    }

    if (cpi->refresh_golden_frame) {
      ref_cnt_fb(cm->frame_bufs,
                 &cm->ref_frame_map[cpi->gld_fb_idx], cm->new_fb_idx);
      if (!cpi->rc.is_src_frame_alt_ref)
        vpx_memcpy(cpi->interp_filter_selected[GOLDEN_FRAME],
                   cpi->interp_filter_selected[0],
                   sizeof(cpi->interp_filter_selected[0]));
      else
        vpx_memcpy(cpi->interp_filter_selected[GOLDEN_FRAME],
                   cpi->interp_filter_selected[ALTREF_FRAME],
                   sizeof(cpi->interp_filter_selected[ALTREF_FRAME]));
    }
  }

  if (cpi->refresh_last_frame) {
#if CONFIG_MULTI_REF
    if (cpi->refresh_last2_frame) {
      if (cpi->refresh_last3_frame) {
        if (cpi->refresh_last4_frame) {
            if (cm->frame_type == KEY_FRAME)
              ref_cnt_fb(cm->frame_bufs,
                         &cm->ref_frame_map[cpi->lst4_fb_idx],
                         cm->new_fb_idx);
            else
              ref_cnt_fb(cm->frame_bufs,
                         &cm->ref_frame_map[cpi->lst4_fb_idx],
                         cm->ref_frame_map[cpi->lst3_fb_idx]);
        }

        if (cm->frame_type == KEY_FRAME)
          ref_cnt_fb(cm->frame_bufs,
                     &cm->ref_frame_map[cpi->lst3_fb_idx],
                     cm->new_fb_idx);
        else
          ref_cnt_fb(cm->frame_bufs,
                     &cm->ref_frame_map[cpi->lst3_fb_idx],
                     cm->ref_frame_map[cpi->lst2_fb_idx]);
      }

      if (cm->frame_type == KEY_FRAME)
        ref_cnt_fb(cm->frame_bufs,
                   &cm->ref_frame_map[cpi->lst2_fb_idx],
                   cm->new_fb_idx);
      else
        ref_cnt_fb(cm->frame_bufs,
                   &cm->ref_frame_map[cpi->lst2_fb_idx],
                   cm->ref_frame_map[cpi->lst_fb_idx]);
    }
#endif  // CONFIG_MULTI_REF
    ref_cnt_fb(cm->frame_bufs,
               &cm->ref_frame_map[cpi->lst_fb_idx], cm->new_fb_idx);

    if (!cpi->rc.is_src_frame_alt_ref) {
#if CONFIG_MULTI_REF
      if (cpi->refresh_last2_frame) {
        if (cpi->refresh_last3_frame) {
          if (cpi->refresh_last4_frame) {
            if (cm->frame_type == KEY_FRAME)
              vpx_memcpy(cpi->interp_filter_selected[LAST4_FRAME],
                         cpi->interp_filter_selected[0],
                         sizeof(cpi->interp_filter_selected[0]));
            else
              vpx_memcpy(cpi->interp_filter_selected[LAST4_FRAME],
                         cpi->interp_filter_selected[LAST3_FRAME],
                         sizeof(cpi->interp_filter_selected[LAST3_FRAME]));
          }

          if (cm->frame_type == KEY_FRAME)
            vpx_memcpy(cpi->interp_filter_selected[LAST3_FRAME],
                       cpi->interp_filter_selected[0],
                       sizeof(cpi->interp_filter_selected[0]));
          else
            vpx_memcpy(cpi->interp_filter_selected[LAST3_FRAME],
                       cpi->interp_filter_selected[LAST2_FRAME],
                       sizeof(cpi->interp_filter_selected[LAST2_FRAME]));
        }

        if (cm->frame_type == KEY_FRAME)
          vpx_memcpy(cpi->interp_filter_selected[LAST2_FRAME],
                     cpi->interp_filter_selected[0],
                     sizeof(cpi->interp_filter_selected[0]));
        else
          vpx_memcpy(cpi->interp_filter_selected[LAST2_FRAME],
                     cpi->interp_filter_selected[LAST_FRAME],
                     sizeof(cpi->interp_filter_selected[LAST_FRAME]));
      }
#endif  // CONFIG_MULTI_REF
      vpx_memcpy(cpi->interp_filter_selected[LAST_FRAME],
                 cpi->interp_filter_selected[0],
                 sizeof(cpi->interp_filter_selected[0]));
    }
  }

#if CONFIG_VP9_TEMPORAL_DENOISING
  if (cpi->oxcf.noise_sensitivity > 0) {
    vp9_denoiser_update_frame_info(&cpi->denoiser,
                                   *cpi->Source,
                                   cpi->common.frame_type,
                                   cpi->refresh_alt_ref_frame,
                                   cpi->refresh_golden_frame,
                                   cpi->refresh_last_frame);
  }
#endif  // CONFIG_VP9_TEMPORAL_DENOISING
}

static void loopfilter_frame(VP9_COMP *cpi, VP9_COMMON *cm) {
  MACROBLOCKD *xd = &cpi->mb.e_mbd;
  struct loopfilter *lf = &cm->lf;
  if (xd->lossless) {
    lf->filter_level = 0;
  } else {
    struct vpx_usec_timer timer;

    vp9_clear_system_state();

    vpx_usec_timer_start(&timer);

    vp9_pick_filter_level(cpi->Source, cpi, cpi->sf.lpf_pick);

    vpx_usec_timer_mark(&timer);
    cpi->time_pick_lpf += vpx_usec_timer_elapsed(&timer);
  }

  if (lf->filter_level > 0) {
    vp9_loop_filter_frame(cm->frame_to_show, cm, xd, lf->filter_level,
                          0, 0);
  }
#if CONFIG_LOOP_POSTFILTER
  vp9_loop_bilateral_init(&cm->lf_info, cm->lf.bilateral_level,
                          cm->frame_type == KEY_FRAME);
  if (cm->lf_info.bilateral_used)
    vp9_loop_bilateral_rows(cm->frame_to_show, cm, 0, cm->mi_rows, 0);
#endif  // CONFIG_LOOP_POSTFILTER

  vp9_extend_frame_inner_borders(cm->frame_to_show);
}

void vp9_scale_references(VP9_COMP *cpi) {
  VP9_COMMON *cm = &cpi->common;
  MV_REFERENCE_FRAME ref_frame;
  const VP9_REFFRAME ref_mask[REFS_PER_FRAME] = {
    VP9_LAST_FLAG,
#if CONFIG_MULTI_REF
    VP9_LAST2_FLAG,
    VP9_LAST3_FLAG,
    VP9_LAST4_FLAG,
#endif  // CONFIG_MULTI_REF
    VP9_GOLD_FLAG,
    VP9_ALT_FLAG
  };

  for (ref_frame = LAST_FRAME; ref_frame <= ALTREF_FRAME; ++ref_frame) {
    const int idx = cm->ref_frame_map[get_ref_frame_idx(cpi, ref_frame)];
    const YV12_BUFFER_CONFIG *const ref = &cm->frame_bufs[idx].buf;

    // Need to convert from VP9_REFFRAME to index into ref_mask (subtract 1).
    if ((cpi->ref_frame_flags & ref_mask[ref_frame - 1]) &&
        (ref->y_crop_width != cm->width || ref->y_crop_height != cm->height)) {
      const int new_fb = get_free_fb(cm);
      vp9_realloc_frame_buffer(&cm->frame_bufs[new_fb].buf,
                               cm->width, cm->height,
                               cm->subsampling_x, cm->subsampling_y,
#if CONFIG_VP9_HIGHBITDEPTH
                               cm->use_highbitdepth,
#endif  // CONFIG_VP9_HIGHBITDEPTH
                               VP9_ENC_BORDER_IN_PIXELS, NULL, NULL, NULL);
#if CONFIG_VP9_HIGHBITDEPTH
      scale_and_extend_frame(ref, &cm->frame_bufs[new_fb].buf,
                             (int)cm->bit_depth);
#else
      scale_and_extend_frame(ref, &cm->frame_bufs[new_fb].buf);
#endif  // CONFIG_VP9_HIGHBITDEPTH
      cpi->scaled_ref_idx[ref_frame - 1] = new_fb;
    } else {
      cpi->scaled_ref_idx[ref_frame - 1] = idx;
      cm->frame_bufs[idx].ref_count++;
    }
  }
}

static void release_scaled_references(VP9_COMP *cpi) {
  VP9_COMMON *cm = &cpi->common;
  int i;

  for (i = 0; i < REFS_PER_FRAME; i++)
    cm->frame_bufs[cpi->scaled_ref_idx[i]].ref_count--;
}

static void full_to_model_count(unsigned int *model_count,
                                unsigned int *full_count) {
  int n;
  model_count[ZERO_TOKEN] = full_count[ZERO_TOKEN];
  model_count[ONE_TOKEN] = full_count[ONE_TOKEN];
  model_count[TWO_TOKEN] = full_count[TWO_TOKEN];
  for (n = THREE_TOKEN; n < EOB_TOKEN; ++n)
    model_count[TWO_TOKEN] += full_count[n];
  model_count[EOB_MODEL_TOKEN] = full_count[EOB_TOKEN];
}

static void full_to_model_counts(vp9_coeff_count_model *model_count,
                                 vp9_coeff_count *full_count) {
  int i, j, k, l;

  for (i = 0; i < PLANE_TYPES; ++i)
    for (j = 0; j < REF_TYPES; ++j)
      for (k = 0; k < COEF_BANDS; ++k)
        for (l = 0; l < BAND_COEFF_CONTEXTS(k); ++l)
          full_to_model_count(model_count[i][j][k][l], full_count[i][j][k][l]);
}

#if 0 && CONFIG_INTERNAL_STATS
static void output_frame_level_debug_stats(VP9_COMP *cpi) {
  VP9_COMMON *const cm = &cpi->common;
  FILE *const f = fopen("tmp.stt", cm->current_video_frame ? "a" : "w");
  int recon_err;

  vp9_clear_system_state();

  recon_err = vp9_get_y_sse(cpi->Source, get_frame_new_buffer(cm));

  if (cpi->twopass.total_left_stats.coded_error != 0.0)
    fprintf(f, "%10u %10d %10d %10d %10d"
        "%10"PRId64" %10"PRId64" %10"PRId64" %10"PRId64" %10d "
        "%7.2lf %7.2lf %7.2lf %7.2lf %7.2lf"
        "%6d %6d %5d %5d %5d "
        "%10"PRId64" %10.3lf"
        "%10lf %8u %10d %10d %10d\n",
        cpi->common.current_video_frame, cpi->rc.this_frame_target,
        cpi->rc.projected_frame_size,
        cpi->rc.projected_frame_size / cpi->common.MBs,
        (cpi->rc.projected_frame_size - cpi->rc.this_frame_target),
        cpi->rc.vbr_bits_off_target,
        cpi->rc.total_target_vs_actual,
        (cpi->rc.starting_buffer_level - cpi->rc.bits_off_target),
        cpi->rc.total_actual_bits, cm->base_qindex,
        vp9_convert_qindex_to_q(cm->base_qindex, cm->bit_depth),
        (double)vp9_dc_quant(cm->base_qindex, 0, cm->bit_depth) / 4.0,
        vp9_convert_qindex_to_q(cpi->twopass.active_worst_quality,
                                cm->bit_depth),
        cpi->rc.avg_q,
        vp9_convert_qindex_to_q(cpi->oxcf.cq_level, cm->bit_depth),
        cpi->refresh_last_frame, cpi->refresh_golden_frame,
        cpi->refresh_alt_ref_frame, cm->frame_type, cpi->rc.gfu_boost,
        cpi->twopass.bits_left,
        cpi->twopass.total_left_stats.coded_error,
        cpi->twopass.bits_left /
            (1 + cpi->twopass.total_left_stats.coded_error),
        cpi->tot_recode_hits, recon_err, cpi->rc.kf_boost,
        cpi->twopass.kf_zeromotion_pct);

  fclose(f);

  if (0) {
    FILE *const fmodes = fopen("Modes.stt", "a");
    int i;

    fprintf(fmodes, "%6d:%1d:%1d:%1d ", cpi->common.current_video_frame,
            cm->frame_type, cpi->refresh_golden_frame,
            cpi->refresh_alt_ref_frame);

    for (i = 0; i < MAX_MODES; ++i)
      fprintf(fmodes, "%5d ", cpi->mode_chosen_counts[i]);

    fprintf(fmodes, "\n");

    fclose(fmodes);
  }
}
#endif

static void encode_without_recode_loop(VP9_COMP *cpi,
                                       int q) {
  VP9_COMMON *const cm = &cpi->common;
  vp9_clear_system_state();
  vp9_set_quantizer(cm, q);
  setup_frame(cpi);
  // Variance adaptive and in frame q adjustment experiments are mutually
  // exclusive.
  if (cpi->oxcf.aq_mode == VARIANCE_AQ) {
    vp9_vaq_frame_setup(cpi);
  } else if (cpi->oxcf.aq_mode == COMPLEXITY_AQ) {
    vp9_setup_in_frame_q_adj(cpi);
  } else if (cpi->oxcf.aq_mode == CYCLIC_REFRESH_AQ) {
    vp9_cyclic_refresh_setup(cpi);
  }

  // transform / motion compensation build reconstruction frame
  vp9_encode_frame(cpi);

  // Update the skip mb flag probabilities based on the distribution
  // seen in the last encoder iteration.
  // update_base_skip_probs(cpi);
  vp9_clear_system_state();
}

static void encode_with_recode_loop(VP9_COMP *cpi,
                                    size_t *size,
                                    uint8_t *dest,
                                    int q,
                                    int bottom_index,
                                    int top_index) {
  VP9_COMMON *const cm = &cpi->common;
  RATE_CONTROL *const rc = &cpi->rc;
  int loop_count = 0;
  int loop = 0;
  int overshoot_seen = 0;
  int undershoot_seen = 0;
  int q_low = bottom_index, q_high = top_index;
  int frame_over_shoot_limit;
  int frame_under_shoot_limit;

  // Decide frame size bounds
  vp9_rc_compute_frame_size_bounds(cpi, rc->this_frame_target,
                                   &frame_under_shoot_limit,
                                   &frame_over_shoot_limit);

  do {
    vp9_clear_system_state();

    vp9_set_quantizer(cm, q);

#if !CONFIG_QCTX_TPROBS
    if (loop_count == 0)
#endif  // CONFIG_QCTX_TPROBS
      setup_frame(cpi);

#if CONFIG_PALETTE
    if (loop_count == 0 && frame_is_intra_only(cm))
      cm->allow_palette_mode = 1;
#endif  // CONFIG_PALETTE
#if CONFIG_INTRABC
    if (loop_count == 0 && frame_is_intra_only(cm))
      cm->allow_intrabc_mode = 1;
#endif  // CONFIG_INTRABC

    // Variance adaptive and in frame q adjustment experiments are mutually
    // exclusive.
    if (cpi->oxcf.aq_mode == VARIANCE_AQ) {
      vp9_vaq_frame_setup(cpi);
    } else if (cpi->oxcf.aq_mode == COMPLEXITY_AQ) {
      vp9_setup_in_frame_q_adj(cpi);
    }

#if CONFIG_PALETTE
    vp9_free_palette_map(cm);
#endif  // CONFIG_PALETTE
    // transform / motion compensation build reconstruction frame

    vp9_encode_frame(cpi);

    // Update the skip mb flag probabilities based on the distribution
    // seen in the last encoder iteration.
    // update_base_skip_probs(cpi);

    vp9_clear_system_state();

    // Dummy pack of the bitstream using up to date stats to get an
    // accurate estimate of output frame size to determine if we need
    // to recode.
    if (cpi->sf.recode_loop >= ALLOW_RECODE_KFARFGF) {
      save_coding_context(cpi);

#if CONFIG_ROW_TILE
      vp9_pack_bitstream(cpi, dest, size, 0);
#else
      vp9_pack_bitstream(cpi, dest, size);
#endif

      rc->projected_frame_size = (int)(*size) << 3;
      restore_coding_context(cpi);

      if (frame_over_shoot_limit == 0)
        frame_over_shoot_limit = 1;
    }

    if (cpi->oxcf.rc_mode == VPX_Q) {
      loop = 0;
    } else {
      if ((cm->frame_type == KEY_FRAME) &&
           rc->this_key_frame_forced &&
           (rc->projected_frame_size < rc->max_frame_bandwidth)) {
        int last_q = q;
        int kf_err;

        int high_err_target = cpi->ambient_err;
        int low_err_target = cpi->ambient_err >> 1;

#if CONFIG_VP9_HIGHBITDEPTH
        if (cm->use_highbitdepth) {
          kf_err = vp9_highbd_get_y_sse(cpi->Source, get_frame_new_buffer(cm),
                                        cm->bit_depth);
        } else {
          kf_err = vp9_get_y_sse(cpi->Source, get_frame_new_buffer(cm));
        }
#else
        kf_err = vp9_get_y_sse(cpi->Source, get_frame_new_buffer(cm));
#endif  // CONFIG_VP9_HIGHBITDEPTH

        // Prevent possible divide by zero error below for perfect KF
        kf_err += !kf_err;

        // The key frame is not good enough or we can afford
        // to make it better without undue risk of popping.
        if ((kf_err > high_err_target &&
             rc->projected_frame_size <= frame_over_shoot_limit) ||
            (kf_err > low_err_target &&
             rc->projected_frame_size <= frame_under_shoot_limit)) {
          // Lower q_high
          q_high = q > q_low ? q - 1 : q_low;

          // Adjust Q
          q = (q * high_err_target) / kf_err;
          q = MIN(q, (q_high + q_low) >> 1);
        } else if (kf_err < low_err_target &&
                   rc->projected_frame_size >= frame_under_shoot_limit) {
          // The key frame is much better than the previous frame
          // Raise q_low
          q_low = q < q_high ? q + 1 : q_high;

          // Adjust Q
          q = (q * low_err_target) / kf_err;
          q = MIN(q, (q_high + q_low + 1) >> 1);
        }

        // Clamp Q to upper and lower limits:
        q = clamp(q, q_low, q_high);

        loop = q != last_q;
      } else if (recode_loop_test(
          cpi, frame_over_shoot_limit, frame_under_shoot_limit,
          q, MAX(q_high, top_index), bottom_index)) {
        // Is the projected frame size out of range and are we allowed
        // to attempt to recode.
        int last_q = q;
        int retries = 0;

        // Frame size out of permitted range:
        // Update correction factor & compute new Q to try...

        // Frame is too large
        if (rc->projected_frame_size > rc->this_frame_target) {
          // Special case if the projected size is > the max allowed.
          if (rc->projected_frame_size >= rc->max_frame_bandwidth)
            q_high = rc->worst_quality;

          // Raise Qlow as to at least the current value
          q_low = q < q_high ? q + 1 : q_high;

          if (undershoot_seen || loop_count > 1) {
            // Update rate_correction_factor unless
            vp9_rc_update_rate_correction_factors(cpi, 1);

            q = (q_high + q_low + 1) / 2;
          } else {
            // Update rate_correction_factor unless
            vp9_rc_update_rate_correction_factors(cpi, 0);

            q = vp9_rc_regulate_q(cpi, rc->this_frame_target,
                                   bottom_index, MAX(q_high, top_index));

            while (q < q_low && retries < 10) {
              vp9_rc_update_rate_correction_factors(cpi, 0);
              q = vp9_rc_regulate_q(cpi, rc->this_frame_target,
                                     bottom_index, MAX(q_high, top_index));
              retries++;
            }
          }

          overshoot_seen = 1;
        } else {
          // Frame is too small
          q_high = q > q_low ? q - 1 : q_low;

          if (overshoot_seen || loop_count > 1) {
            vp9_rc_update_rate_correction_factors(cpi, 1);
            q = (q_high + q_low) / 2;
          } else {
            vp9_rc_update_rate_correction_factors(cpi, 0);
            q = vp9_rc_regulate_q(cpi, rc->this_frame_target,
                                   bottom_index, top_index);
            // Special case reset for qlow for constrained quality.
            // This should only trigger where there is very substantial
            // undershoot on a frame and the auto cq level is above
            // the user passsed in value.
            if (cpi->oxcf.rc_mode == VPX_CQ &&
                q < q_low) {
              q_low = q;
            }

            while (q > q_high && retries < 10) {
              vp9_rc_update_rate_correction_factors(cpi, 0);
              q = vp9_rc_regulate_q(cpi, rc->this_frame_target,
                                     bottom_index, top_index);
              retries++;
            }
          }

          undershoot_seen = 1;
        }

        // Clamp Q to upper and lower limits:
        q = clamp(q, q_low, q_high);

        loop = q != last_q;
      } else {
        loop = 0;
      }
    }

    // Special case for overlay frame.
    if (rc->is_src_frame_alt_ref &&
        rc->projected_frame_size < rc->max_frame_bandwidth)
      loop = 0;

#if CONFIG_PALETTE
    if (frame_is_intra_only(cm) && cm->allow_palette_mode &&
        cm->palette_counter * 100 < cm->palette_blocks_signalled) {
      cm->allow_palette_mode = 0;
      loop = 1;
    }
#endif  // CONFIG_PALETTE
#if CONFIG_INTRABC
    if (frame_is_intra_only(cm) && cm->allow_intrabc_mode &&
        cm->intrabc_counter * 100 < cm->intrabc_blocks_signalled) {
      cm->allow_intrabc_mode = 0;
      loop = 1;
    }
#endif  // CONFIG_INTRABC

    if (loop) {
      loop_count++;

#if CONFIG_INTERNAL_STATS
      cpi->tot_recode_hits++;
#endif
    }
  } while (loop);
}

static int get_ref_frame_flags(const VP9_COMP *cpi) {
  const int *const map = cpi->common.ref_frame_map;

  const int gold_is_last = map[cpi->gld_fb_idx] == map[cpi->lst_fb_idx];
  const int alt_is_last = map[cpi->alt_fb_idx] == map[cpi->lst_fb_idx];
  const int gold_is_alt = map[cpi->gld_fb_idx] == map[cpi->alt_fb_idx];

#if CONFIG_MULTI_REF
  const int last2_is_last = map[cpi->lst2_fb_idx] == map[cpi->lst_fb_idx];
  const int gld_is_last2 = map[cpi->gld_fb_idx] == map[cpi->lst2_fb_idx];
  const int alt_is_last2 = map[cpi->alt_fb_idx] == map[cpi->lst2_fb_idx];

  const int last3_is_last = map[cpi->lst3_fb_idx] == map[cpi->lst_fb_idx];
  const int last3_is_last2 = map[cpi->lst3_fb_idx] == map[cpi->lst2_fb_idx];
  const int gld_is_last3 = map[cpi->gld_fb_idx] == map[cpi->lst3_fb_idx];
  const int alt_is_last3 = map[cpi->alt_fb_idx] == map[cpi->lst3_fb_idx];

  const int last4_is_last = map[cpi->lst4_fb_idx] == map[cpi->lst_fb_idx];
  const int last4_is_last2 = map[cpi->lst4_fb_idx] == map[cpi->lst2_fb_idx];
  const int last4_is_last3 = map[cpi->lst4_fb_idx] == map[cpi->lst3_fb_idx];
  const int gld_is_last4 = map[cpi->gld_fb_idx] == map[cpi->lst4_fb_idx];
  const int alt_is_last4 = map[cpi->alt_fb_idx] == map[cpi->lst4_fb_idx];
#endif  // CONFIG_MULTI_REF

  int flags = VP9_ALT_FLAG | VP9_GOLD_FLAG | VP9_LAST_FLAG;
#if CONFIG_MULTI_REF
  flags |= VP9_LAST2_FLAG;
  flags |= VP9_LAST3_FLAG;
  flags |= VP9_LAST4_FLAG;
#endif  // CONFIG_MULTI_REF

  if (gold_is_last)
    flags &= ~VP9_GOLD_FLAG;

  if (cpi->rc.frames_till_gf_update_due == INT_MAX)
    flags &= ~VP9_GOLD_FLAG;

  if (alt_is_last)
    flags &= ~VP9_ALT_FLAG;

  if (gold_is_alt)
    flags &= ~VP9_ALT_FLAG;

#if CONFIG_MULTI_REF

  if (last4_is_last || last4_is_last2 || last4_is_last3)
    flags &= ~VP9_LAST4_FLAG;

  if (gld_is_last4)
    flags &= ~VP9_GOLD_FLAG;

  if (alt_is_last4)
    flags &= ~VP9_ALT_FLAG;

  if (last3_is_last || last3_is_last2)
    flags &= ~VP9_LAST3_FLAG;

  if (gld_is_last3)
    flags &= ~VP9_GOLD_FLAG;

  if (alt_is_last3)
    flags &= ~VP9_ALT_FLAG;

  if (last2_is_last)
    flags &= ~VP9_LAST2_FLAG;

  if (gld_is_last2)
    flags &= ~VP9_GOLD_FLAG;

  if (alt_is_last2)
    flags &= ~VP9_ALT_FLAG;
#endif  // CONFIG_MULTI_REF

  return flags;
}

static void set_ext_overrides(VP9_COMP *cpi) {
  // Overrides the defaults with the externally supplied values with
  // vp9_update_reference() and vp9_update_entropy() calls
  // Note: The overrides are valid only for the next frame passed
  // to encode_frame_to_data_rate() function
  if (cpi->ext_refresh_frame_context_pending) {
    cpi->common.refresh_frame_context = cpi->ext_refresh_frame_context;
    cpi->ext_refresh_frame_context_pending = 0;
  }
  if (cpi->ext_refresh_frame_flags_pending) {
    cpi->refresh_last_frame = cpi->ext_refresh_last_frame;
#if CONFIG_MULTI_REF
    cpi->refresh_last2_frame = cpi->ext_refresh_last2_frame;
    cpi->refresh_last3_frame = cpi->ext_refresh_last3_frame;
    cpi->refresh_last4_frame = cpi->ext_refresh_last4_frame;
#endif  // CONFIG_MULTI_REF
    cpi->refresh_golden_frame = cpi->ext_refresh_golden_frame;
    cpi->refresh_alt_ref_frame = cpi->ext_refresh_alt_ref_frame;
    cpi->ext_refresh_frame_flags_pending = 0;
  }
}

YV12_BUFFER_CONFIG *vp9_scale_if_required(VP9_COMMON *cm,
                                          YV12_BUFFER_CONFIG *unscaled,
                                          YV12_BUFFER_CONFIG *scaled) {
  if (cm->mi_cols * MI_SIZE != unscaled->y_width ||
      cm->mi_rows * MI_SIZE != unscaled->y_height) {
#if CONFIG_VP9_HIGHBITDEPTH
    scale_and_extend_frame_nonnormative(unscaled, scaled, (int)cm->bit_depth);
#else
    scale_and_extend_frame_nonnormative(unscaled, scaled);
#endif  // CONFIG_VP9_HIGHBITDEPTH
    return scaled;
  } else {
    return unscaled;
  }
}

static int is_skippable_frame(const VP9_COMP *cpi) {
  // If the current frame does not have non-zero motion vector detected in the
  // first  pass, and so do its previous and forward frames, then this frame
  // can be skipped for partition check, and the partition size is assigned
  // according to the variance
  const TWO_PASS *const twopass = &cpi->twopass;

  return (!frame_is_intra_only(&cpi->common) &&
    twopass->stats_in - 2 > twopass->stats_in_start &&
    twopass->stats_in < twopass->stats_in_end &&
    (twopass->stats_in - 1)->pcnt_inter - (twopass->stats_in - 1)->pcnt_motion
    == 1 &&
    (twopass->stats_in - 2)->pcnt_inter - (twopass->stats_in - 2)->pcnt_motion
    == 1 &&
    twopass->stats_in->pcnt_inter - twopass->stats_in->pcnt_motion == 1);
}

static void set_arf_sign_bias(VP9_COMP *cpi) {
  VP9_COMMON *const cm = &cpi->common;
  int arf_sign_bias;

  if ((cpi->oxcf.pass == 2) && cpi->multi_arf_allowed) {
    const GF_GROUP *const gf_group = &cpi->twopass.gf_group;
    arf_sign_bias = cpi->rc.source_alt_ref_active &&
                    (!cpi->refresh_alt_ref_frame ||
                     (gf_group->rf_level[gf_group->index] == GF_ARF_LOW));
  } else {
    arf_sign_bias =
      (cpi->rc.source_alt_ref_active && !cpi->refresh_alt_ref_frame);
  }
  cm->ref_frame_sign_bias[ALTREF_FRAME] = arf_sign_bias;
}

static void set_mv_search_params(VP9_COMP *cpi) {
  const VP9_COMMON *const cm = &cpi->common;
  const unsigned int max_mv_def = MIN(cm->width, cm->height);

  // Default based on max resolution.
  cpi->mv_step_param = vp9_init_search_range(max_mv_def);

  if (cpi->sf.mv.auto_mv_step_size) {
    if (frame_is_intra_only(cm)) {
      // Initialize max_mv_magnitude for use in the first INTER frame
      // after a key/intra-only frame.
      cpi->max_mv_magnitude = max_mv_def;
    } else {
      if (cm->show_frame)
        // Allow mv_steps to correspond to twice the max mv magnitude found
        // in the previous frame, capped by the default max_mv_magnitude based
        // on resolution.
        cpi->mv_step_param =
            vp9_init_search_range(MIN(max_mv_def, 2 * cpi->max_mv_magnitude));
      cpi->max_mv_magnitude = 0;
    }
  }
}


int setup_interp_filter_search_mask(VP9_COMP *cpi) {
  INTERP_FILTER ifilter;
  int ref_total[MAX_REF_FRAMES] = {0};
  MV_REFERENCE_FRAME ref;
  int mask = 0;

  if (cpi->common.last_frame_type == KEY_FRAME ||
      cpi->refresh_alt_ref_frame)
    return mask;

  for (ref = LAST_FRAME; ref <= ALTREF_FRAME; ++ref)
    for (ifilter = EIGHTTAP; ifilter <= EIGHTTAP_SHARP; ++ifilter)
      ref_total[ref] += cpi->interp_filter_selected[ref][ifilter];

  for (ifilter = EIGHTTAP; ifilter <= EIGHTTAP_SHARP; ++ifilter) {
    if ((ref_total[LAST_FRAME] &&
        cpi->interp_filter_selected[LAST_FRAME][ifilter] == 0) &&
#if CONFIG_MULTI_REF
        (ref_total[LAST2_FRAME] == 0 ||
         cpi->interp_filter_selected[LAST2_FRAME][ifilter] * 50
         < ref_total[LAST2_FRAME]) &&
        (ref_total[LAST3_FRAME] == 0 ||
         cpi->interp_filter_selected[LAST3_FRAME][ifilter] * 50
         < ref_total[LAST3_FRAME]) &&
        (ref_total[LAST4_FRAME] == 0 ||
         cpi->interp_filter_selected[LAST4_FRAME][ifilter] * 50
         < ref_total[LAST4_FRAME]) &&
#endif  // CONFIG_MULTI_REF
        (ref_total[GOLDEN_FRAME] == 0 ||
         cpi->interp_filter_selected[GOLDEN_FRAME][ifilter] * 50
         < ref_total[GOLDEN_FRAME]) &&
        (ref_total[ALTREF_FRAME] == 0 ||
         cpi->interp_filter_selected[ALTREF_FRAME][ifilter] * 50
         < ref_total[ALTREF_FRAME]))
      mask |= 1 << ifilter;
  }
  return mask;
}

static void encode_frame_to_data_rate(VP9_COMP *cpi,
                                      size_t *size,
                                      uint8_t *dest,
                                      unsigned int *frame_flags) {
  VP9_COMMON *const cm = &cpi->common;
  const VP9EncoderConfig *const oxcf = &cpi->oxcf;
  struct segmentation *const seg = &cm->seg;
  TX_SIZE t;
  int q;
  int top_index;
  int bottom_index;

  set_ext_overrides(cpi);

  cpi->Source = vp9_scale_if_required(cm, cpi->un_scaled_source,
                                      &cpi->scaled_source);

  if (cpi->unscaled_last_source != NULL)
    cpi->Last_Source = vp9_scale_if_required(cm, cpi->unscaled_last_source,
                                             &cpi->scaled_last_source);

  vp9_scale_references(cpi);

  vp9_clear_system_state();

  // Set the arf sign bias for this frame.
  set_arf_sign_bias(cpi);

  // Set default state for segment based loop filter update flags.
  cm->lf.mode_ref_delta_update = 0;

  set_mv_search_params(cpi);

  if (cpi->oxcf.pass == 2 &&
      cpi->sf.adaptive_interp_filter_search)
    cpi->sf.interp_filter_search_mask =
        setup_interp_filter_search_mask(cpi);

  // Set various flags etc to special state if it is a key frame.
  if (frame_is_intra_only(cm)) {
    // Reset the loop filter deltas and segmentation map.
    vp9_reset_segment_features(&cm->seg);

    // If segmentation is enabled force a map update for key frames.
    if (seg->enabled) {
      seg->update_map = 1;
      seg->update_data = 1;
    }

    // The alternate reference frame cannot be active for a key frame.
    cpi->rc.source_alt_ref_active = 0;

    cm->error_resilient_mode = oxcf->error_resilient_mode;
    cm->frame_parallel_decoding_mode = oxcf->frame_parallel_decoding_mode;

    // By default, encoder assumes decoder can use prev_mi.
    if (cm->error_resilient_mode) {
      cm->frame_parallel_decoding_mode = 1;
      cm->reset_frame_context = 0;
      cm->refresh_frame_context = 0;
    } else if (cm->intra_only) {
      // Only reset the current context.
      cm->reset_frame_context = 2;
    }
  }

#if CONFIG_ROW_TILE
  cm->tile_size_bytes = 4;
  cm->tile_col_size_bytes = 4;

  cpi->max_tile_size = INT_MAX;
  cpi->max_tile_col_size = INT_MAX;
#endif

  // Configure experimental use of segmentation for enhanced coding of
  // static regions if indicated.
  // Only allowed in second pass of two pass (as requires lagged coding)
  // and if the relevant speed feature flag is set.
  if (oxcf->pass == 2 && cpi->sf.static_segmentation)
    configure_static_seg_features(cpi);

  // Check if the current frame is skippable for the partition search in the
  // second pass according to the first pass stats
  if (cpi->sf.allow_partition_search_skip && oxcf->pass == 2) {
    cpi->partition_search_skippable_frame = is_skippable_frame(cpi);
  }

  // For 1 pass CBR, check if we are dropping this frame.
  // Never drop on key frame.
  if (oxcf->pass == 0 &&
      oxcf->rc_mode == VPX_CBR &&
      cm->frame_type != KEY_FRAME) {
    if (vp9_rc_drop_frame(cpi)) {
      vp9_rc_postencode_update_drop_frame(cpi);
      ++cm->current_video_frame;
      return;
    }
  }

  vp9_clear_system_state();

#if CONFIG_VP9_POSTPROC
  if (oxcf->noise_sensitivity > 0) {
    int l = 0;
    switch (oxcf->noise_sensitivity) {
      case 1:
        l = 20;
        break;
      case 2:
        l = 40;
        break;
      case 3:
        l = 60;
        break;
      case 4:
      case 5:
        l = 100;
        break;
      case 6:
        l = 150;
        break;
    }
    vp9_denoise(cpi->Source, cpi->Source, l);
  }
#endif

#if CONFIG_INTERNAL_STATS
  {
    int i;
    for (i = 0; i < MAX_MODES; ++i)
      cpi->mode_chosen_counts[i] = 0;
  }
#endif

  vp9_set_speed_features(cpi);

  vp9_set_rd_speed_thresholds(cpi);
  vp9_set_rd_speed_thresholds_sub8x8(cpi);

  // Decide q and q bounds.
  q = vp9_rc_pick_q_and_bounds(cpi, &bottom_index, &top_index);

  if (!frame_is_intra_only(cm)) {
    cm->interp_filter = cpi->sf.default_interp_filter;
    /* TODO: Decide this more intelligently */
    vp9_set_high_precision_mv(cpi, q < HIGH_PRECISION_MV_QTHRESH);
  }

  if (cpi->sf.recode_loop == DISALLOW_RECODE) {
    encode_without_recode_loop(cpi, q);
  } else {
    encode_with_recode_loop(cpi, size, dest, q, bottom_index, top_index);
  }

#if CONFIG_VP9_TEMPORAL_DENOISING
#ifdef OUTPUT_YUV_DENOISED
  if (oxcf->noise_sensitivity > 0) {
    vp9_write_yuv_frame_420(&cpi->denoiser.running_avg_y[INTRA_FRAME],
                            yuv_denoised_file);
  }
#endif
#endif

  // Special case code to reduce pulsing when key frames are forced at a
  // fixed interval. Note the reconstruction error if it is the frame before
  // the force key frame
  if (cpi->rc.next_key_frame_forced && cpi->rc.frames_to_key == 1) {
#if CONFIG_VP9_HIGHBITDEPTH
    if (cm->use_highbitdepth) {
      cpi->ambient_err = vp9_highbd_get_y_sse(cpi->Source,
                                              get_frame_new_buffer(cm),
                                              cm->bit_depth);
    } else {
      cpi->ambient_err = vp9_get_y_sse(cpi->Source, get_frame_new_buffer(cm));
    }
#else
    cpi->ambient_err = vp9_get_y_sse(cpi->Source, get_frame_new_buffer(cm));
#endif  // CONFIG_VP9_HIGHBITDEPTH
  }

  // If the encoder forced a KEY_FRAME decision
  if (cm->frame_type == KEY_FRAME)
    cpi->refresh_last_frame = 1;

  cm->frame_to_show = get_frame_new_buffer(cm);

  // Pick the loop filter level for the frame.
  loopfilter_frame(cpi, cm);

  // printf("Bilateral level: %d\n", cm->lf.bilateral_level);

  // build the bitstream
#if CONFIG_ROW_TILE
  if (vp9_pack_bitstream(cpi, dest, size, 1) < 0) {
    // If the above bitstream packing fails, reset max_tile_size and
    // max_tile_col_size, and do another packing.
    restore_coding_context(cpi);
    cpi->max_tile_size = UINT_MAX;
    cpi->max_tile_col_size = UINT_MAX;
    vp9_pack_bitstream(cpi, dest, size, 1);
  }
#else
  vp9_pack_bitstream(cpi, dest, size);
#endif

#if CONFIG_PALETTE
  vp9_free_palette_map(cm);
#endif  // CONFIG_PALETTE

  if (cm->seg.update_map)
    update_reference_segmentation_map(cpi);

  release_scaled_references(cpi);

  vp9_update_reference_frames(cpi);

  for (t = TX_4X4; t < TX_SIZES; t++)
    full_to_model_counts(cm->counts.coef[t], cpi->coef_counts[t]);

  if (!cm->error_resilient_mode && !cm->frame_parallel_decoding_mode)
    vp9_adapt_coef_probs(cm);

  if (!frame_is_intra_only(cm)) {
    if (!cm->error_resilient_mode && !cm->frame_parallel_decoding_mode) {
      vp9_adapt_mode_probs(cm);
      vp9_adapt_mv_probs(cm, cm->allow_high_precision_mv);
    }
  }

  if (cpi->refresh_golden_frame == 1)
    cpi->frame_flags |= FRAMEFLAGS_GOLDEN;
  else
    cpi->frame_flags &= ~FRAMEFLAGS_GOLDEN;

  if (cpi->refresh_alt_ref_frame == 1)
    cpi->frame_flags |= FRAMEFLAGS_ALTREF;
  else
    cpi->frame_flags &= ~FRAMEFLAGS_ALTREF;

  cpi->ref_frame_flags = get_ref_frame_flags(cpi);

#if CONFIG_MULTI_REF
  cm->last3_frame_type = cm->last2_frame_type;
  cm->last2_frame_type = cm->last_frame_type;
#endif  // CONFIG_MULTI_REF
  cm->last_frame_type = cm->frame_type;

#if CONFIG_LOOP_POSTFILTER
  if (cm->frame_type != KEY_FRAME)
    cm->lf.last_bilateral_level = cm->lf.bilateral_level;
  else
    cm->lf.last_bilateral_level = 0;
#endif

  vp9_rc_postencode_update(cpi, *size);

#if 0
  output_frame_level_debug_stats(cpi);
#endif

  if (cm->frame_type == KEY_FRAME) {
    // Tell the caller that the frame was coded as a key frame
    *frame_flags = cpi->frame_flags | FRAMEFLAGS_KEY;
  } else {
    *frame_flags = cpi->frame_flags & ~FRAMEFLAGS_KEY;
  }

  // Clear the one shot update flags for segmentation map and mode/ref loop
  // filter deltas.
  cm->seg.update_map = 0;
  cm->seg.update_data = 0;
  cm->lf.mode_ref_delta_update = 0;

  // keep track of the last coded dimensions
  cm->last_width = cm->width;
  cm->last_height = cm->height;

  // reset to normal state now that we are done.
  if (!cm->show_existing_frame)
    cm->last_show_frame = cm->show_frame;

  if (cm->show_frame) {
    vp9_swap_mi_and_prev_mi(cm);

    // Don't increment frame counters if this was an altref buffer
    // update not a real frame
    ++cm->current_video_frame;
  }
}

static void Pass0Encode(VP9_COMP *cpi, size_t *size, uint8_t *dest,
                        unsigned int *frame_flags) {
  if (cpi->oxcf.rc_mode == VPX_CBR) {
    vp9_rc_get_one_pass_cbr_params(cpi);
  } else {
    vp9_rc_get_one_pass_vbr_params(cpi);
  }
  encode_frame_to_data_rate(cpi, size, dest, frame_flags);
}

static void Pass2Encode(VP9_COMP *cpi, size_t *size,
                        uint8_t *dest, unsigned int *frame_flags) {
  cpi->allow_encode_breakout = ENCODE_BREAKOUT_ENABLED;
  encode_frame_to_data_rate(cpi, size, dest, frame_flags);
  vp9_twopass_postencode_update(cpi);
}

static void init_motion_estimation(VP9_COMP *cpi) {
  int y_stride = cpi->scaled_source.y_stride;

  if (cpi->sf.mv.search_method == NSTEP) {
    vp9_init3smotion_compensation(&cpi->ss_cfg, y_stride);
  } else if (cpi->sf.mv.search_method == DIAMOND) {
    vp9_init_dsmotion_compensation(&cpi->ss_cfg, y_stride);
  }
}

static void check_initial_width(VP9_COMP *cpi,
#if CONFIG_VP9_HIGHBITDEPTH
                                int use_highbitdepth,
#endif
                                int subsampling_x, int subsampling_y) {
  VP9_COMMON *const cm = &cpi->common;

  if (!cpi->initial_width) {
    cm->subsampling_x = subsampling_x;
    cm->subsampling_y = subsampling_y;
#if CONFIG_VP9_HIGHBITDEPTH
    cm->use_highbitdepth = use_highbitdepth;
#endif

    alloc_raw_frame_buffers(cpi);
    alloc_ref_frame_buffers(cpi);
    alloc_util_frame_buffers(cpi);

    init_motion_estimation(cpi);

    cpi->initial_width = cm->width;
    cpi->initial_height = cm->height;
  }
}


int vp9_receive_raw_frame(VP9_COMP *cpi, unsigned int frame_flags,
                          YV12_BUFFER_CONFIG *sd, int64_t time_stamp,
                          int64_t end_time) {
  VP9_COMMON *cm = &cpi->common;
  struct vpx_usec_timer timer;
  int res = 0;
  const int subsampling_x = sd->subsampling_x;
  const int subsampling_y = sd->subsampling_y;
#if CONFIG_VP9_HIGHBITDEPTH
  const int use_highbitdepth = sd->flags & YV12_FLAG_HIGHBITDEPTH;
  check_initial_width(cpi, use_highbitdepth, subsampling_x, subsampling_y);
#else
  check_initial_width(cpi, subsampling_x, subsampling_y);
#endif  // CONFIG_VP9_HIGHBITDEPTH

  vpx_usec_timer_start(&timer);

  if (vp9_lookahead_push(cpi->lookahead, sd, time_stamp, end_time, frame_flags))
    res = -1;
  vpx_usec_timer_mark(&timer);
  cpi->time_receive_data += vpx_usec_timer_elapsed(&timer);

  if ((cm->profile == PROFILE_0 || cm->profile == PROFILE_2) &&
      (subsampling_x != 1 || subsampling_y != 1)) {
    vpx_internal_error(&cm->error, VPX_CODEC_INVALID_PARAM,
                       "Non-4:2:0 color space requires profile 1 or 3");
    res = -1;
  }
  if ((cm->profile == PROFILE_1 || cm->profile == PROFILE_3) &&
      (subsampling_x == 1 && subsampling_y == 1)) {
    vpx_internal_error(&cm->error, VPX_CODEC_INVALID_PARAM,
                       "4:2:0 color space requires profile 0 or 2");
    res = -1;
  }

  return res;
}

static int frame_is_reference(const VP9_COMP *cpi) {
  const VP9_COMMON *cm = &cpi->common;

  return cm->frame_type == KEY_FRAME ||
         cpi->refresh_last_frame ||
#if CONFIG_MULTI_REF
         cpi->refresh_last2_frame ||
         cpi->refresh_last3_frame ||
         cpi->refresh_last4_frame ||
#endif  // CONFIG_MULTI_REF
         cpi->refresh_golden_frame ||
         cpi->refresh_alt_ref_frame ||
         cm->refresh_frame_context ||
         cm->lf.mode_ref_delta_update ||
         cm->seg.update_map ||
         cm->seg.update_data;
}

void adjust_frame_rate(VP9_COMP *cpi,
                       const struct lookahead_entry *source) {
  int64_t this_duration;
  int step = 0;

  if (source->ts_start == cpi->first_time_stamp_ever) {
    this_duration = source->ts_end - source->ts_start;
    step = 1;
  } else {
    int64_t last_duration = cpi->last_end_time_stamp_seen
        - cpi->last_time_stamp_seen;

    this_duration = source->ts_end - cpi->last_end_time_stamp_seen;

    // do a step update if the duration changes by 10%
    if (last_duration)
      step = (int)((this_duration - last_duration) * 10 / last_duration);
  }

  if (this_duration) {
    if (step) {
      vp9_new_framerate(cpi, 10000000.0 / this_duration);
    } else {
      // Average this frame's rate into the last second's average
      // frame rate. If we haven't seen 1 second yet, then average
      // over the whole interval seen.
      const double interval = MIN((double)(source->ts_end
                                   - cpi->first_time_stamp_ever), 10000000.0);
      double avg_duration = 10000000.0 / cpi->framerate;
      avg_duration *= (interval - avg_duration + this_duration);
      avg_duration /= interval;

      vp9_new_framerate(cpi, 10000000.0 / avg_duration);
    }
  }
  cpi->last_time_stamp_seen = source->ts_start;
  cpi->last_end_time_stamp_seen = source->ts_end;
}

// Returns 0 if this is not an alt ref else the offset of the source frame
// used as the arf midpoint.
static int get_arf_src_index(VP9_COMP *cpi) {
  RATE_CONTROL *const rc = &cpi->rc;
  int arf_src_index = 0;
  if (is_altref_enabled(cpi)) {
    if (cpi->oxcf.pass == 2) {
      const GF_GROUP *const gf_group = &cpi->twopass.gf_group;
      if (gf_group->update_type[gf_group->index] == ARF_UPDATE) {
        arf_src_index = gf_group->arf_src_offset[gf_group->index];
      }
    } else if (rc->source_alt_ref_pending) {
      arf_src_index = rc->frames_till_gf_update_due;
    }
  }
  return arf_src_index;
}

static void check_src_altref(VP9_COMP *cpi,
                             const struct lookahead_entry *source) {
  RATE_CONTROL *const rc = &cpi->rc;

  if (cpi->oxcf.pass == 2) {
    const GF_GROUP *const gf_group = &cpi->twopass.gf_group;
    rc->is_src_frame_alt_ref =
      (gf_group->update_type[gf_group->index] == OVERLAY_UPDATE);
  } else {
    rc->is_src_frame_alt_ref = cpi->alt_ref_source &&
                               (source == cpi->alt_ref_source);
  }

  if (rc->is_src_frame_alt_ref) {
    // Current frame is an ARF overlay frame.
    cpi->alt_ref_source = NULL;

    // Don't refresh the last buffer for an ARF overlay frame. It will
    // become the GF so preserve last as an alternative prediction option.
    cpi->refresh_last_frame = 0;
  }
}

int vp9_get_compressed_data(VP9_COMP *cpi, unsigned int *frame_flags,
                            size_t *size, uint8_t *dest,
                            int64_t *time_stamp, int64_t *time_end, int flush) {
  const VP9EncoderConfig *const oxcf = &cpi->oxcf;
  VP9_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &cpi->mb.e_mbd;
  RATE_CONTROL *const rc = &cpi->rc;
  struct vpx_usec_timer  cmptimer;
  YV12_BUFFER_CONFIG *force_src_buffer = NULL;
  struct lookahead_entry *last_source = NULL;
  struct lookahead_entry *source = NULL;
  MV_REFERENCE_FRAME ref_frame;
  int arf_src_index;

  vpx_usec_timer_start(&cmptimer);

  vp9_set_high_precision_mv(cpi, ALTREF_HIGH_PRECISION_MV);

  // Is multi-arf enabled.
  if ((oxcf->pass == 2) &&
      (cpi->oxcf.enable_auto_arf > 1) && (cpi->oxcf.rc_mode == VPX_VBR))
    cpi->multi_arf_allowed = 1;
  else
    cpi->multi_arf_allowed = 0;
  cpi->multi_arf_last_grp_enabled = 0;

  // Normal defaults
  cm->reset_frame_context = 0;
  cm->refresh_frame_context = 1;

  cpi->refresh_last_frame = 1;
#if CONFIG_MULTI_REF
  cpi->refresh_last2_frame = 0;
  cpi->refresh_last3_frame = 0;
  cpi->refresh_last4_frame = 0;
#endif  // CONFIG_MULTI_REF
  cpi->refresh_golden_frame = 0;
  cpi->refresh_alt_ref_frame = 0;

  // Should we encode an arf frame.
  arf_src_index = get_arf_src_index(cpi);

  if (arf_src_index) {
    assert(arf_src_index <= rc->frames_to_key);

    if ((source = vp9_lookahead_peek(cpi->lookahead, arf_src_index)) != NULL) {
      cpi->alt_ref_source = source;

      if (oxcf->arnr_max_frames > 0) {
        // Produce the filtered ARF frame.
        vp9_temporal_filter(cpi, arf_src_index);
        vp9_extend_frame_borders(&cpi->alt_ref_buffer);
        force_src_buffer = &cpi->alt_ref_buffer;
      }

      cm->show_frame = 0;
      cpi->refresh_alt_ref_frame = 1;
      cpi->refresh_golden_frame = 0;
      cpi->refresh_last_frame = 0;
#if CONFIG_MULTI_REF
      cpi->refresh_last2_frame = 0;
      cpi->refresh_last3_frame = 0;
      cpi->refresh_last4_frame = 0;
#endif  // CONFIG_MULTI_REF
      rc->is_src_frame_alt_ref = 0;
      rc->source_alt_ref_pending = 0;
    } else {
      rc->source_alt_ref_pending = 0;
    }
  }

  if (!source) {
    // Get last frame source.
    if (cm->current_video_frame > 0) {
      if ((last_source = vp9_lookahead_peek(cpi->lookahead, -1)) == NULL)
        return -1;
    }

    // Read in the source frame.
    source = vp9_lookahead_pop(cpi->lookahead, flush);
    if (source != NULL) {
      cm->show_frame = 1;
      cm->intra_only = 0;

      // Check to see if the frame should be encoded as an arf overlay.
      check_src_altref(cpi, source);
    }
  }

  if (source) {
    cpi->un_scaled_source = cpi->Source = force_src_buffer ? force_src_buffer
                                                           : &source->img;

    cpi->unscaled_last_source = last_source != NULL ? &last_source->img : NULL;

    *time_stamp = source->ts_start;
    *time_end = source->ts_end;
    *frame_flags = (source->flags & VPX_EFLAG_FORCE_KF) ? FRAMEFLAGS_KEY : 0;

  } else {
    *size = 0;
    if (flush && oxcf->pass == 1 && !cpi->twopass.first_pass_done) {
      vp9_end_first_pass(cpi);    /* get last stats packet */
      cpi->twopass.first_pass_done = 1;
    }
    return -1;
  }

  if (source->ts_start < cpi->first_time_stamp_ever) {
    cpi->first_time_stamp_ever = source->ts_start;
    cpi->last_end_time_stamp_seen = source->ts_start;
  }

  // Clear down mmx registers
  vp9_clear_system_state();

  // adjust frame rates based on timestamps given
  if (cm->show_frame) {
    adjust_frame_rate(cpi, source);
  }

  // start with a 0 size frame
  *size = 0;

  /* find a free buffer for the new frame, releasing the reference previously
   * held.
   */
  if (cm->frame_bufs[cm->new_fb_idx].ref_count > 0)
    cm->frame_bufs[cm->new_fb_idx].ref_count--;
  cm->new_fb_idx = get_free_fb(cm);

  // For two pass encodes analyse the first pass stats and determine
  // the bit allocation and other parameters for this frame / group of frames.
  if (oxcf->pass == 2) {
    vp9_rc_get_second_pass_params(cpi);
  }

  if (cpi->multi_arf_allowed) {
    if (cm->frame_type == KEY_FRAME) {
      init_buffer_indices(cpi);
    } else if (oxcf->pass == 2) {
      const GF_GROUP *const gf_group = &cpi->twopass.gf_group;
      cpi->alt_fb_idx = gf_group->arf_ref_idx[gf_group->index];
    }
  }

  cpi->frame_flags = *frame_flags;

  if (oxcf->pass == 2 &&
      cm->current_video_frame == 0 &&
      oxcf->allow_spatial_resampling &&
      oxcf->rc_mode == VPX_VBR) {
    // Internal scaling is triggered on the first frame.
    vp9_set_size_literal(cpi, oxcf->scaled_frame_width,
                         oxcf->scaled_frame_height);
  }

  // Reset the frame pointers to the current frame size
  vp9_realloc_frame_buffer(get_frame_new_buffer(cm),
                           cm->width, cm->height,
                           cm->subsampling_x, cm->subsampling_y,
#if CONFIG_VP9_HIGHBITDEPTH
                           cm->use_highbitdepth,
#endif
                           VP9_ENC_BORDER_IN_PIXELS, NULL, NULL, NULL);

  alloc_util_frame_buffers(cpi);
  init_motion_estimation(cpi);

  for (ref_frame = LAST_FRAME; ref_frame <= ALTREF_FRAME; ++ref_frame) {
    const int idx = cm->ref_frame_map[get_ref_frame_idx(cpi, ref_frame)];
    YV12_BUFFER_CONFIG *const buf = &cm->frame_bufs[idx].buf;
    RefBuffer *const ref_buf = &cm->frame_refs[ref_frame - 1];
    ref_buf->buf = buf;
    ref_buf->idx = idx;
#if CONFIG_VP9_HIGHBITDEPTH
    vp9_setup_scale_factors_for_frame(&ref_buf->sf,
                                      buf->y_crop_width, buf->y_crop_height,
                                      cm->width, cm->height,
                                      (buf->flags & YV12_FLAG_HIGHBITDEPTH) ?
                                      1 : 0);
#else
    vp9_setup_scale_factors_for_frame(&ref_buf->sf,
                                      buf->y_crop_width, buf->y_crop_height,
                                      cm->width, cm->height);
#endif  // CONFIG_VP9_HIGHBITDEPTH
    if (vp9_is_scaled(&ref_buf->sf))
      vp9_extend_frame_borders(buf);
  }

  set_ref_ptrs(cm, xd, LAST_FRAME, LAST_FRAME);

  if (oxcf->aq_mode == VARIANCE_AQ) {
    vp9_vaq_init();
  }

  if (oxcf->pass == 1) {
    const int lossless = is_lossless_requested(oxcf);
#if CONFIG_VP9_HIGHBITDEPTH
    if (cpi->oxcf.use_highbitdepth)
      cpi->mb.fwd_txm4x4 = lossless ? vp9_highbd_fwht4x4 : vp9_highbd_fdct4x4;
    else
      cpi->mb.fwd_txm4x4 = lossless ? vp9_fwht4x4 : vp9_fdct4x4;
    cpi->mb.highbd_itxm_add = lossless ? vp9_highbd_iwht4x4_add :
                                         vp9_highbd_idct4x4_add;
#else
    cpi->mb.fwd_txm4x4 = lossless ? vp9_fwht4x4 : vp9_fdct4x4;
#endif  // CONFIG_VP9_HIGHBITDEPTH
    cpi->mb.itxm_add = lossless ? vp9_iwht4x4_add : vp9_idct4x4_add;
#if CONFIG_SR_MODE
    cpi->mb.itxm = lossless ? vp9_iwht4x4 : vp9_idct4x4;
#endif  // CONFIG_SR_MODE
    vp9_first_pass(cpi, source);
  } else if (oxcf->pass == 2) {
    Pass2Encode(cpi, size, dest, frame_flags);
  } else {
    // One pass encode
    Pass0Encode(cpi, size, dest, frame_flags);
  }

  if (cm->refresh_frame_context)
    cm->frame_contexts[cm->frame_context_idx] = cm->fc;

  // Frame was dropped, release scaled references.
  if (*size == 0) {
    release_scaled_references(cpi);
  }

  if (*size > 0) {
    cpi->droppable = !frame_is_reference(cpi);
  }

  vpx_usec_timer_mark(&cmptimer);
  cpi->time_compress_data += vpx_usec_timer_elapsed(&cmptimer);

  if (cpi->b_calculate_psnr && oxcf->pass != 1 && cm->show_frame)
    generate_psnr_packet(cpi);

#if CONFIG_INTERNAL_STATS

  if (oxcf->pass != 1) {
    cpi->bytes += (int)(*size);

    if (cm->show_frame) {
      cpi->count++;

      if (cpi->b_calculate_psnr) {
        YV12_BUFFER_CONFIG *orig = cpi->Source;
        YV12_BUFFER_CONFIG *recon = cm->frame_to_show;
        YV12_BUFFER_CONFIG *pp = &cm->post_proc_buffer;
        PSNR_STATS psnr;
#if CONFIG_VP9_HIGHBITDEPTH
        calc_highbd_psnr(orig, recon, &psnr, cpi->mb.e_mbd.bd,
                         cpi->oxcf.input_bit_depth);
#else
        calc_psnr(orig, recon, &psnr);
#endif  // CONFIG_VP9_HIGHBITDEPTH

        cpi->total += psnr.psnr[0];
        cpi->total_y += psnr.psnr[1];
        cpi->total_u += psnr.psnr[2];
        cpi->total_v += psnr.psnr[3];
        cpi->total_sq_error += psnr.sse[0];
        cpi->total_samples += psnr.samples[0];

        {
          PSNR_STATS psnr2;
          double frame_ssim2 = 0, weight = 0;
#if CONFIG_VP9_POSTPROC
          // TODO(agrange) Add resizing of post-proc buffer in here when the
          // encoder is changed to use on-demand buffer allocation.
          vp9_deblock(cm->frame_to_show, &cm->post_proc_buffer,
                      cm->lf.filter_level * 10 / 6);
#endif  // CONFIG_VP9_POSTPROC
          vp9_clear_system_state();

#if CONFIG_VP9_HIGHBITDEPTH
          calc_highbd_psnr(orig, pp, &psnr2, cpi->mb.e_mbd.bd,
                           cpi->oxcf.input_bit_depth);
#else
          calc_psnr(orig, pp, &psnr2);
#endif  // CONFIG_VP9_HIGHBITDEPTH

          cpi->totalp += psnr2.psnr[0];
          cpi->totalp_y += psnr2.psnr[1];
          cpi->totalp_u += psnr2.psnr[2];
          cpi->totalp_v += psnr2.psnr[3];
          cpi->totalp_sq_error += psnr2.sse[0];
          cpi->totalp_samples += psnr2.samples[0];

#if CONFIG_VP9_HIGHBITDEPTH
          if (cm->use_highbitdepth) {
            frame_ssim2 = vp9_highbd_calc_ssim(orig, recon, &weight, xd->bd);
          } else {
            frame_ssim2 = vp9_calc_ssim(orig, recon, &weight);
          }
#else
          frame_ssim2 = vp9_calc_ssim(orig, recon, &weight);
#endif  // CONFIG_VP9_HIGHBITDEPTH

          cpi->summed_quality += frame_ssim2 * weight;
          cpi->summed_weights += weight;

#if CONFIG_VP9_HIGHBITDEPTH
          if (cm->use_highbitdepth) {
            frame_ssim2 = vp9_highbd_calc_ssim(
                orig, &cm->post_proc_buffer, &weight, xd->bd);
          } else {
            frame_ssim2 = vp9_calc_ssim(orig, &cm->post_proc_buffer, &weight);
          }
#else
          frame_ssim2 = vp9_calc_ssim(orig, &cm->post_proc_buffer, &weight);
#endif  // CONFIG_VP9_HIGHBITDEPTH

          cpi->summedp_quality += frame_ssim2 * weight;
          cpi->summedp_weights += weight;
#if 0
          {
            FILE *f = fopen("q_used.stt", "a");
            fprintf(f, "%5d : Y%f7.3:U%f7.3:V%f7.3:F%f7.3:S%7.3f\n",
                    cpi->common.current_video_frame, y2, u2, v2,
                    frame_psnr2, frame_ssim2);
            fclose(f);
          }
#endif  // 0
        }
      }


      if (cpi->b_calculate_ssimg) {
        double y, u, v, frame_all;
#if CONFIG_VP9_HIGHBITDEPTH
        if (cm->use_highbitdepth) {
          frame_all = vp9_highbd_calc_ssimg(cpi->Source, cm->frame_to_show, &y,
                                            &u, &v, xd->bd);
        } else {
          frame_all = vp9_calc_ssimg(cpi->Source, cm->frame_to_show, &y, &u,
                                     &v);
        }
#else
        frame_all = vp9_calc_ssimg(cpi->Source, cm->frame_to_show, &y, &u, &v);
#endif  // CONFIG_VP9_HIGHBITDEPTH
        cpi->total_ssimg_y += y;
        cpi->total_ssimg_u += u;
        cpi->total_ssimg_v += v;
        cpi->total_ssimg_all += frame_all;
      }
    }
  }

#endif  // CONFIG_INTERNAL_STATS

  return 0;
}

int vp9_get_preview_raw_frame(VP9_COMP *cpi, YV12_BUFFER_CONFIG *dest,
                              vp9_ppflags_t *flags) {
  VP9_COMMON *cm = &cpi->common;
#if !CONFIG_VP9_POSTPROC
  (void)flags;
#endif

  if (!cm->show_frame) {
    return -1;
  } else {
    int ret;
#if CONFIG_VP9_POSTPROC
    ret = vp9_post_proc_frame(cm, dest, flags);
#else
    if (cm->frame_to_show) {
      *dest = *cm->frame_to_show;
      dest->y_width = cm->width;
      dest->y_height = cm->height;
      dest->uv_width = cm->width >> cm->subsampling_x;
      dest->uv_height = cm->height >> cm->subsampling_y;
      ret = 0;
    } else {
      ret = -1;
    }
#endif  // !CONFIG_VP9_POSTPROC
    vp9_clear_system_state();
    return ret;
  }
}

int vp9_set_active_map(VP9_COMP *cpi, unsigned char *map, int rows, int cols) {
  if (rows == cpi->common.mb_rows && cols == cpi->common.mb_cols) {
    const int mi_rows = cpi->common.mi_rows;
    const int mi_cols = cpi->common.mi_cols;
    if (map) {
      int r, c;
      for (r = 0; r < mi_rows; r++) {
        for (c = 0; c < mi_cols; c++) {
          cpi->segmentation_map[r * mi_cols + c] =
              !map[(r >> 1) * cols + (c >> 1)];
        }
      }
      vp9_enable_segfeature(&cpi->common.seg, 1, SEG_LVL_SKIP);
      vp9_enable_segmentation(&cpi->common.seg);
    } else {
      vp9_disable_segmentation(&cpi->common.seg);
    }
    return 0;
  } else {
    return -1;
  }
}

int vp9_set_internal_size(VP9_COMP *cpi,
                          VPX_SCALING horiz_mode, VPX_SCALING vert_mode) {
  VP9_COMMON *cm = &cpi->common;
  int hr = 0, hs = 0, vr = 0, vs = 0;

  if (horiz_mode > ONETWO || vert_mode > ONETWO)
    return -1;

  Scale2Ratio(horiz_mode, &hr, &hs);
  Scale2Ratio(vert_mode, &vr, &vs);

  // always go to the next whole number
  cm->width = (hs - 1 + cpi->oxcf.width * hr) / hs;
  cm->height = (vs - 1 + cpi->oxcf.height * vr) / vs;
  assert(cm->width <= cpi->initial_width);
  assert(cm->height <= cpi->initial_height);

  update_frame_size(cpi);

  return 0;
}

int vp9_set_size_literal(VP9_COMP *cpi, unsigned int width,
                         unsigned int height) {
  VP9_COMMON *cm = &cpi->common;
#if CONFIG_VP9_HIGHBITDEPTH
  check_initial_width(cpi, 1, 1, cm->use_highbitdepth);
#else
  check_initial_width(cpi, 1, 1);
#endif  // CONFIG_VP9_HIGHBITDEPTH

  if (width) {
    cm->width = width;
    if (cm->width > cpi->initial_width) {
      cm->width = cpi->initial_width;
      printf("Warning: Desired width too large, changed to %d\n", cm->width);
    }
  }

  if (height) {
    cm->height = height;
    if (cm->height > cpi->initial_height) {
      cm->height = cpi->initial_height;
      printf("Warning: Desired height too large, changed to %d\n", cm->height);
    }
  }
  assert(cm->width <= cpi->initial_width);
  assert(cm->height <= cpi->initial_height);

  update_frame_size(cpi);

  return 0;
}

int vp9_get_y_sse(const YV12_BUFFER_CONFIG *a, const YV12_BUFFER_CONFIG *b) {
  assert(a->y_crop_width == b->y_crop_width);
  assert(a->y_crop_height == b->y_crop_height);

  return (int)get_sse(a->y_buffer, a->y_stride, b->y_buffer, b->y_stride,
                      a->y_crop_width, a->y_crop_height);
}

#if CONFIG_VP9_HIGHBITDEPTH
int vp9_highbd_get_y_sse(const YV12_BUFFER_CONFIG *a,
                         const YV12_BUFFER_CONFIG *b,
                         vpx_bit_depth_t bit_depth) {
  unsigned int sse;
  int sum;
  assert(a->y_crop_width == b->y_crop_width);
  assert(a->y_crop_height == b->y_crop_height);
  assert((a->flags & YV12_FLAG_HIGHBITDEPTH) != 0);
  assert((b->flags & YV12_FLAG_HIGHBITDEPTH) != 0);
  switch (bit_depth) {
    case VPX_BITS_8:
      highbd_variance(a->y_buffer, a->y_stride, b->y_buffer, b->y_stride,
                      a->y_crop_width, a->y_crop_height, &sse, &sum);
      return (int) sse;
    case VPX_BITS_10:
      highbd_10_variance(a->y_buffer, a->y_stride, b->y_buffer, b->y_stride,
                         a->y_crop_width, a->y_crop_height, &sse, &sum);
      return (int) sse;
    case VPX_BITS_12:
      highbd_12_variance(a->y_buffer, a->y_stride, b->y_buffer, b->y_stride,
                         a->y_crop_width, a->y_crop_height, &sse, &sum);
      return (int) sse;
    default:
      assert(0 && "bit_depth should be VPX_BITS_8, VPX_BITS_10 or VPX_BITS_12");
      return -1;
  }
}
#endif  // CONFIG_VP9_HIGHBITDEPTH

int vp9_get_quantizer(VP9_COMP *cpi) {
  return cpi->common.base_qindex;
}

void vp9_apply_encoding_flags(VP9_COMP *cpi, vpx_enc_frame_flags_t flags) {
  if (flags & (VP8_EFLAG_NO_REF_LAST | VP8_EFLAG_NO_REF_GF |
               VP8_EFLAG_NO_REF_ARF)) {
    int ref = 7;

    if (flags & VP8_EFLAG_NO_REF_LAST)
      ref ^= VP9_LAST_FLAG;

    if (flags & VP8_EFLAG_NO_REF_GF)
      ref ^= VP9_GOLD_FLAG;

    if (flags & VP8_EFLAG_NO_REF_ARF)
      ref ^= VP9_ALT_FLAG;

    vp9_use_as_reference(cpi, ref);
  }

  if (flags & (VP8_EFLAG_NO_UPD_LAST | VP8_EFLAG_NO_UPD_GF |
               VP8_EFLAG_NO_UPD_ARF | VP8_EFLAG_FORCE_GF |
               VP8_EFLAG_FORCE_ARF)) {
    int upd = 7;

    if (flags & VP8_EFLAG_NO_UPD_LAST)
      upd ^= VP9_LAST_FLAG;

    if (flags & VP8_EFLAG_NO_UPD_GF)
      upd ^= VP9_GOLD_FLAG;

    if (flags & VP8_EFLAG_NO_UPD_ARF)
      upd ^= VP9_ALT_FLAG;

    vp9_update_reference(cpi, upd);
  }

  if (flags & VP8_EFLAG_NO_UPD_ENTROPY) {
    vp9_update_entropy(cpi, 0);
  }
}
