/*
 * Jobs (plan section 7.7), version 1: one job per context, for the default
 * framebuffer. A job is one binning pass plus one render pass. Ported from
 * PiStorm3D's gl_FrameBegin / gl_FramePresent (gl/src/context.c), with these
 * differences:
 *
 *  - The GPU submission lock (section 7.1, phase 1) is held from the binning
 *    submit until the render has finished, and the flush waits for the render
 *    before returning (PiStorm3D defers that wait to the next frame).
 *  - Lock order: GPU lock, then the bitmap lock, so a program waiting for its
 *    turn on the GPU never holds a bitmap lock. While the bitmap is locked
 *    nothing is logged (cybergraphics: no library calls under a bitmap lock;
 *    the exec cache and semaphore calls are unavoidable, as in PiStorm3D).
 *  - Depth and stencil of the default framebuffer are cleared by the hardware
 *    at the start of every tile and neither loaded nor stored (section 7.2:
 *    EGL leaves them undefined after a swap). This replaces PiStorm3D's
 *    depth_persist_seen heuristic and its one possibly corrupted frame.
 *  - No intermediate passes yet: nothing draws in the first M1 step.
 */
#include <proto/exec.h>
#include "ogles2_internal.h"
#include "v3d_context.h"
#include "v3d_commands.h"
#include "v3d_clbuf.h"
#include "v3d_hw.h"
#include "v3d_submit_timeout.h"
#include "v3d_gpu_lock.h"

#define GPU_LOCK_TIMEOUT_MS 5000

extern v3d_u32 g_v3d_cl_generation; /* v3d_frame.c: bumped whenever a CL slot is reset */

/* Starts the job's binning list if none is open. */
void ogles2_job_begin(struct OGLES2Context *ctx)
{
    V3DContext *v = ctx->v3d;
    v3d_u8 s;

    if (v->frame_active)
        return;
    s = v->build_slot;
    v3d_cl_reset(&v->tile_list_mem[s], &v->tile_list_buf[s]);
    v3d_cl_reset(&v->binning_mem[s], &v->binning_buf[s]);
    v3d_cl_reset(&v->render_mem[s], &v->render_buf[s]);
    v3d_cl_reset(&v->state_mem[s], &v->state_buf[s]);
    g_v3d_cl_generation++;

    v->current_buf = &v->binning_buf[s];
    SetBuffer(v, &v->binning_buf[s]);
    /* As Mesa's v3d_start_binning (v3dx_draw.c). */
    NumberOfLayers(v, 1);
    TileBinningModeCfg(v, v3d_TILE_ALLOCATION_INITIAL_BLOCK_SIZE_64B,
                       v3d_TILE_ALLOCATION_BLOCK_SIZE_64B, 1, V3D_INTERNAL_BPP_32, FALSE, FALSE,
                       v->width, v->height);
    DoCommand(v, v3d_OP_FLUSH_VCD_CACHE);
    OcclusionQueryCounter(v, 0);
    DoCommand(v, v3d_OP_START_TILE_BINNING);

    v->frame_active = TRUE;
    ctx->job_serial++;
    v->ez_state = V3D_EZ_UNDECIDED;
    v->first_ez_state = V3D_EZ_UNDECIDED;
    v->pending_clear_color = FALSE;
    v->pending_clear_depth = FALSE;
    v->frame_corrupted = FALSE;
    v->draw_state_configured = FALSE;
}

static v3d_u8 to_byte(GLfloat f)
{
    return (v3d_u8)(f * 255.0f + 0.5f); /* f is clamped to [0,1] by glClearColor */
}

/* glClear's whole-framebuffer path: the tile buffer is cleared when the job
 * renders. The values are those current at the glClear (Mesa snapshots them
 * the same way), not at the flush. */
void ogles2_job_clear(struct OGLES2Context *ctx, GLbitfield mask)
{
    V3DContext *v = ctx->v3d;
    const GLfloat *c = ctx->state.clear_color;

    ogles2_job_begin(ctx);
    if (mask & GL_COLOR_BUFFER_BIT) {
        v->pending_clear_color = TRUE;
        v->clear_color_value = ((v3d_u32)to_byte(c[3]) << 24) | ((v3d_u32)to_byte(c[0]) << 16) |
                               ((v3d_u32)to_byte(c[1]) << 8) | to_byte(c[2]); /* ARGB */
    }
    if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
        v->pending_clear_depth = TRUE;
        v->clear_depth_value = ctx->state.clear_depth;
        ctx->job_clear_stencil = (UBYTE)ctx->state.clear_stencil; /* the low 8 bits (GLES 2.0 4.2.3) */
    }
}

/* Builds the tile list and the render list. The display must be locked. */
static void build_render(struct OGLES2Context *ctx)
{
    V3DContext *v = ctx->v3d;
    struct OGLES2Display *d = &ctx->display;
    v3d_u8 s = v->build_slot;
    UWORD width = v->width, height = v->height;
    ULONG tilesX = v->frame.tilesX, tilesY = v->frame.tilesY;
    ULONG start, end, sw = 1, sh = 1, fw, fh, sx, sy;
    UBYTE clear_color = v->pending_clear_color;
    UBYTE ez = v->first_ez_state;
    UBYTE ez_off = (ez == V3D_EZ_DISABLED) || (width & 1) || (height & 1); /* GFXH-1918 */
    UBYTE depth_type = ctx->depth_format == OGLES2_DEPTH_16 ? V3D_INTERNAL_TYPE_DEPTH16
                     : ctx->depth_format == OGLES2_DEPTH_24 ? V3D_INTERNAL_TYPE_DEPTH24
                                                            : V3D_INTERNAL_TYPE_DEPTH32F;
    ULONG argb, abgr;
    UBYTE flip_y = (ctx->probe & OGLES2_PROBE_FLIP_Y) ? TRUE : FALSE;

    /* Finish binning. Probe S: the binner signals the render thread, which
     * waits for it (VC4's protocol; packets 7 and 8 exist on V3D 4.2 too). */
    v->current_buf = &v->binning_buf[s];
    SetBuffer(v, &v->binning_buf[s]);
    if (ctx->probe & OGLES2_PROBE_SEMAPHORE)
        DoCommand(v, v3d_OP_INCREMENT_SEMAPHORE);
    DoCommand(v, v3d_OP_FLUSH);

    /* Generic tile list: load colour unless it is cleared, store it to the
     * render bitmap. Depth: early clear, no load, no store (see the top). */
    v->current_buf = &v->tile_list_buf[s];
    SetBuffer(v, &v->tile_list_buf[s]);
    start = CurrentBufferAddress(v);
    DoCommand(v, v3d_OP_TILE_COORDINATES_IMPLICIT);
    /* Probe Y: Flip Y on both, with the height it needs (Mesa's XML: "used
     * for y flip"). */
    if (!clear_color)
        LoadTileBufferGeneral(v, v3d_RENDER_TARGET_0, V3D_MEMORY_FORMAT_RASTER, flip_y,
                              V3D_DECIMATE_MODE_SAMPLE_0, V3D_OUTPUT_IMAGE_FORMAT_RGBA8, FALSE, FALSE,
                              d->rb_swap, d->bprow, flip_y ? height : 0, d->base);
    DoCommand(v, v3d_OP_END_OF_LOADS);
    PrimListFormat(v, v3d_LIST_TRIANGLES, FALSE);
    SetInstanceid(v, 0);
    BranchToImplicitTileList(v, 0);
    /* No dithering into the RGBA8 framebuffer, whatever GL_DITHER says (GLES2
     * leaves dithering to the implementation): the tile buffer holds 8 bits
     * per channel, so there is nothing finer to dither from, and Mesa never
     * sets the store's dither mode. PiStorm3D dithered when GL_DITHER was on;
     * on the A1200 that store turned 0xff into 0x00 and 0xbf into 0xc0 (M1
     * run 2). Probe D (ENV:OGLES2/Probe) brings it back, for that probe only.
     * Dithering matters for 16-bit targets (plan 7.7), which come later. */
    StoreTileBufferGeneral(v, v3d_RENDER_TARGET_0, V3D_MEMORY_FORMAT_RASTER, flip_y,
                           (ctx->probe & OGLES2_PROBE_STORE_DITHER) ? V3D_DITHER_MODE_RGBA : V3D_DITHER_MODE_NONE,
                           V3D_DECIMATE_MODE_SAMPLE_0, V3D_OUTPUT_IMAGE_FORMAT_RGBA8, FALSE, FALSE,
                           d->rb_swap, d->bprow, flip_y ? height : 0, d->base);
    /* Mesa v3dx_rcl.c, GFXH-1461/GFXH-1689: on 4.2 the per-store clear bit is
     * broken for Z/S, so clear with this packet, RTs bit set; Z/S bit clear
     * because the early Z/S clear does that. */
    if (clear_color || v->pending_clear_depth)
        ClearTileBuffers(v, FALSE, TRUE);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    DoCommand(v, v3d_OP_RETURN_FROM_SUB_LIST);
    end = CurrentBufferAddress(v);

    /* Render list. */
    v->current_buf = &v->render_buf[s];
    SetBuffer(v, &v->render_buf[s]);
    if (ctx->probe & OGLES2_PROBE_SEMAPHORE)
        DoCommand(v, v3d_OP_WAIT_ON_SEMAPHORE);
    TileRenderingModeCFGCommon(v, 1, width, height, v3d_RENDER_TARGET_MAXIMUM_32BPP, FALSE, FALSE,
                               ez == V3D_EZ_GT_GE ? v3d_EARLY_Z_DIRECTION_GT_GE
                                                  : v3d_EARLY_Z_DIRECTION_LT_LE,
                               ez_off, depth_type, TRUE /* early Z/S clear */);
    argb = v->clear_color_value;
    abgr = (argb & 0xFF00FF00UL) | ((argb & 0x00FF0000UL) >> 16) | ((argb & 0x000000FFUL) << 16);
    TileRenderingModeCFGClearColorsPart1(v, 0, abgr);
    if (ctx->probe & OGLES2_PROBE_LOG_CLEAR)
        ogles2_log("job: frame %lu, clear %s, ARGB %08lx, packet ABGR %08lx", ctx->frames,
                   clear_color ? "colour" : "no colour", argb, abgr);
    TileRenderingModeCFGColor(v, V3D_INTERNAL_BPP_32, V3D_INTERNAL_TYPE_8, 0);
    TileRenderingModeCFGZSClearValues(v, v->pending_clear_depth ? ctx->job_clear_stencil : 0,
                                      v->pending_clear_depth ? v->clear_depth_value : 1.0f);
    TileListInitialBlockSize(v, v3d_TILE_ALLOCATION_BLOCK_SIZE_64B, TRUE);

    /* Supertiles: fewer than 256 in the frame (Mesa v3dx_rcl.c). */
    for (;;) {
        fw = (tilesX + sw - 1) / sw;
        fh = (tilesY + sh - 1) / sh;
        if (fw * fh < 256)
            break;
        if (sw < sh)
            sw++;
        else
            sh++;
    }
    Multicore_Rendering_Tile_List_Set_Base(v, (ULONG)v->tile_alloc_mem.hostptr, 0);
    MulticoreRenderingSupertileCFG(v, sw, sh, fw, fh, 1, FALSE, FALSE, tilesY, tilesX);

    /* Mesa's two dummy tiles before the real ones (v3dx_rcl.c). */
    TileCoordinates(v, 0, 0);
    DoCommand(v, v3d_OP_END_OF_LOADS);
    StoreTileBufferGeneral(v, v3d_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    ClearTileBuffers(v, FALSE, TRUE);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    TileCoordinates(v, 0, 0);
    DoCommand(v, v3d_OP_END_OF_LOADS);
    StoreTileBufferGeneral(v, v3d_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    DoCommand(v, v3d_OP_FLUSH_VCD_CACHE);

    StartAddressOfGenericTileList(v, start, end);
    for (sy = 0; sy <= (ULONG)(height - 1) / (64 * sh); sy++)
        for (sx = 0; sx <= (ULONG)(width - 1) / (64 * sw); sx++)
            Supertile_Coordinates(v, sx, sy);
    DoCommand(v, v3d_OP_END_OF_RENDERING);
}

static BOOL any_overflow(V3DContext *v)
{
    v3d_u8 s = v->build_slot;
    return v->binning_buf[s].overflowed || v->state_buf[s].overflowed ||
           v->render_buf[s].overflowed || v->tile_list_buf[s].overflowed;
}

/* Renders the open job into the default framebuffer and waits for it.
 * Returns FALSE if the frame was dropped (the reason is logged). */
BOOL ogles2_job_flush(struct OGLES2Context *ctx)
{
    V3DContext *v = ctx->v3d;
    V3DDevice *dev = ogles2_device();
    struct SignalSemaphore *gpu = ogles2_device_gpu_lock();
    v3d_u8 s = v->build_slot;
    v3d_wait_result wb = v3d_wait_result_success, wr = v3d_wait_result_success;
    const char *drop = 0;

    if (!v->frame_active)
        return TRUE;
    if (v->frame_corrupted) {
        drop = "a draw's uniform stream went stale (state buffer grew mid-draw)";
        goto out;
    }
    if (!v3d_gpu_lock(gpu, GPU_LOCK_TIMEOUT_MS)) {
        drop = "the GPU stayed busy (another program holds the submission lock)";
        goto out;
    }
    if (!ogles2_display_lock(&ctx->display)) {
        v3d_gpu_unlock(gpu);
        drop = "the render bitmap could not be locked";
        goto out;
    }

    build_render(ctx);
    if (any_overflow(v)) {
        ogles2_display_unlock(&ctx->display);
        v3d_gpu_unlock(gpu);
        drop = "a control list overflowed";
        goto out;
    }

    v3d_context_flush_for_dma(dev, v);
    v3d_invalidate_caches();
    if (ctx->probe & OGLES2_PROBE_SEMAPHORE) {
        /* Probe S: both lists go to the GPU at once; the render list waits
         * for the binner's semaphore. Out-of-memory service stays in the
         * binning wait. */
        v3d_u8 last = v3d_submit_binning(dev, &v->frame, (v3d_address)(ULONG)v->binning_buf[s].start,
                                         (v3d_address)((ULONG)v->binning_buf[s].start +
                                                       v->binning_buf[s].used),
                                         (v3d_address)(ULONG)v->tile_alloc_mem.hostptr,
                                         (v3d_u32)v->tile_alloc_mem.size,
                                         (v3d_address)(ULONG)v->tile_state_mem.hostptr);
        v3d_u8 frame_no = v3d_submit_render((v3d_address)(ULONG)v->render_buf[s].start,
                                            (v3d_address)((ULONG)v->render_buf[s].start +
                                                          v->render_buf[s].used));
        wb = v3d_wait_binning_timeout(dev, &v->frame, last);
        v3d_frame_render_submitted(&v->frame);
        wr = v3d_wait_render_timeout(frame_no);
        goto waited;
    }
    {
        v3d_u8 last = v3d_submit_binning(dev, &v->frame, (v3d_address)(ULONG)v->binning_buf[s].start,
                                         (v3d_address)((ULONG)v->binning_buf[s].start +
                                                       v->binning_buf[s].used),
                                         (v3d_address)(ULONG)v->tile_alloc_mem.hostptr,
                                         (v3d_u32)v->tile_alloc_mem.size,
                                         (v3d_address)(ULONG)v->tile_state_mem.hostptr);
        wb = v3d_wait_binning_timeout(dev, &v->frame, last);
    }
    if (wb == v3d_wait_result_success) {
        v3d_u8 frame_no;
        v3d_invalidate_caches();
        frame_no = v3d_submit_render((v3d_address)(ULONG)v->render_buf[s].start,
                                     (v3d_address)((ULONG)v->render_buf[s].start +
                                                   v->render_buf[s].used));
        v3d_frame_render_submitted(&v->frame);
        wr = v3d_wait_render_timeout(frame_no);
    }
waited:
    ogles2_display_unlock(&ctx->display);
    v3d_gpu_unlock(gpu);

    if (wb != v3d_wait_result_success)
        drop = wb == v3d_wait_result_timed_out ? "binning timed out" : "binning failed";
    else if (wr != v3d_wait_result_success)
        ogles2_log("job: render %s", (ULONG)(wr == v3d_wait_result_timed_out ? "timed out" : "failed"));
    else
        v3d_frame_retire(dev, &v->frame); /* only after a successful wait */
    ctx->frames++;
    v->build_slot = 1 - v->build_slot;
    g_v3d_cl_generation++;

out:
    v->frame_active = FALSE;
    if (drop) {
        ctx->frames_dropped++;
        ogles2_log("job: frame dropped: %s", (ULONG)drop);
        return FALSE;
    }
    return wr == v3d_wait_result_success;
}

BOOL ogles2_job_has_draws(struct OGLES2Context *ctx)
{
    return ctx->v3d && ctx->v3d->frame_active && ctx->v3d->draw_state_configured;
}

/* aglDestroyContext: forget an open job (nothing was submitted). */
void ogles2_job_discard(struct OGLES2Context *ctx)
{
    if (ctx->v3d)
        ctx->v3d->frame_active = FALSE;
}
