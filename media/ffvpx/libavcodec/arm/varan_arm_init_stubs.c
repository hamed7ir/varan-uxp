/*
 * Varan v1.1 -- deliberate no-op ARM init stubs for ffvpx.
 *
 * THIS FILE IS VARAN'S, NOT UPSTREAM'S. It is named distinctly so nobody mistakes
 * it for a vendored ffmpeg source.
 *
 * WHY IT EXISTS
 * -------------
 * B9 set ARCH_ARM 1 so ffvpx would finally build its ARM SIMD. That woke up every
 * `#if ARCH_ARM` branch in the generic code, including two dispatcher calls whose
 * implementations live in files we did not vendor:
 *
 *   libavcodec/h264pred.c:594   ff_h264_pred_init_arm(h, codec_id, bit_depth, cfi)
 *   libavcodec/videodsp.c:51    ff_videodsp_init_arm(ctx, bpc)
 *
 * Both are pure DISPATCHERS: they only overwrite function pointers in a context
 * that the generic C code has already filled in with correct implementations.
 * Declining to install the ARM variants costs speed, never correctness.
 *
 * WHY STUBS RATHER THAN VENDORING (a deliberate trade, not an oversight)
 * ---------------------------------------------------------------------
 * Vendoring them properly needs five more upstream files --
 *   arm/videodsp_init_arm.c, arm/videodsp_arm.h, arm/videodsp_armv5te.S,
 *   arm/h264pred_init_arm.c, arm/h264pred_neon.S (~19 symbols)
 * -- and this is the FOURTH cascade ARCH_ARM=1 has produced, so the depth beyond
 * those five is not known without another build cycle each time.
 *
 * What they would buy does NOT touch this release's target:
 *   - h264pred NEON accelerates H.264-derived INTRA PREDICTION, which in ffvpx is
 *     used by VP8. VP9 has its own intra prediction and does not call it. YouTube
 *     serves VP9.
 *   - videodsp_armv5te is a PREFETCH hint for emulated-edge motion compensation.
 * Everything that actually decodes VP9 -- mc, loop filter, inverse transform -- IS
 * vendored and building (117 ffvpx ARM objects).
 *
 * And the baseline matters: before v1.1, ARCH_ARM was 0 and ffvpx had NO ARM
 * optimization whatsoever. With these two stubbed we are strictly ahead of v1.0,
 * not behind it.
 *
 * TO UNDO THIS LATER, the work is fully scoped: vendor the five files above, drop
 * this file from libavcodec/arm/moz.build, and relink. Do it if a device trip ever
 * shows VP8 intra prediction mattering.
 *
 * SAFETY NOTE: the signatures below MUST track h264pred.h:121 and videodsp.h:82.
 * They are compiled against those headers, so a future upstream signature change
 * is a compile error here, not a silent mismatch.
 */

#include "libavutil/attributes.h"
#include "libavcodec/h264pred.h"
#include "libavcodec/videodsp.h"

av_cold void ff_h264_pred_init_arm(H264PredContext *h, int codec_id,
                                   const int bit_depth, const int chroma_format_idc)
{
    /* Intentionally empty: keep the generic C predictors installed by
     * ff_h264_pred_init(). See the header comment for why. */
}

av_cold void ff_videodsp_init_arm(VideoDSPContext *ctx, int bpc)
{
    /* Intentionally empty: keep the generic C emulated_edge_mc / prefetch. */
}
