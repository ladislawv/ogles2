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
 *  - A job renders into the default framebuffer or into a framebuffer
 *    object (M5, plan 7.7; fbo.c): ctx->job_fb. Drawing into another
 *    framebuffer renders the open job first (jobs v1). An object's job loads
 *    and stores its colour image in the image's own layout and its depth
 *    and stencil every time, without the Y flip, with tile pools of its own
 *    when it has more tiles than the default framebuffer.
 */
#include <proto/exec.h>
#include "ogles2_internal.h"
#include "v3d_context.h"
#include "v3d_commands.h"
#include "v3d_clbuf.h"
#include "v3d_hw.h"
#include "v3d_submit_timeout.h"
#include "v3d_gpu_lock.h"
#include "v3d_regs.h"

#define GPU_LOCK_TIMEOUT_MS 5000

extern v3d_u32 g_v3d_cl_generation; /* v3d_frame.c: bumped whenever a CL slot is reset */

/* ---- the binning list grows (plan section 9, M4) ----
 * Its block is fixed (V3D_INITIAL_CL_BUFFER_SIZE, 4 MB), and a frame with
 * enough draws outgrows it. Instead of dropping the frame, a claim that
 * does not fit ends the block with a BRANCH to a new one, as Mesa's
 * v3d_cl_ensure_space does; the list is then submitted from the first
 * block to the end of the last. BRANCH_ROOM bytes past `capacity` are kept
 * free in every block for that packet. The new blocks are freed when the
 * frame's render has finished (v3d_frame_defer_free). */
#define BRANCH_ROOM 8                         /* BRANCH: opcode + 32-bit address */
#define CHAIN_BLOCK (1024UL * 1024)           /* the least for a further block */

static void *grow_binning(v3d_static_buffer *buf, v3d_uintptr size)
{
    struct OGLES2Context *ctx = buf->grow_data;
    V3DContext *v = ctx->v3d;
    V3DDevice *dev = ogles2_device();
    v3d_mem m;
    UBYTE *b;
    /* Each block twice the last: few blocks even for huge frames (the
     * frame's retire list holds V3D_RETIRE_MAX). */
    ULONG addr, len, want = (ULONG)buf->capacity * 2;
    if (want < CHAIN_BLOCK)
        want = CHAIN_BLOCK;
    if (want < (ULONG)size + BRANCH_ROOM + 256)
        want = (ULONG)size + BRANCH_ROOM + 256;
    if (v3d_mem_alloc(dev, &m, want) < 0)
        return 0; /* out of memory: the frame is dropped as before */
    /* BRANCH, little-endian, in the room kept free after `used`. */
    b = buf->start + buf->used;
    addr = (ULONG)m.hostptr;
    b[0] = v3d_OP_BRANCH;
    b[1] = (UBYTE)addr;
    b[2] = (UBYTE)(addr >> 8);
    b[3] = (UBYTE)(addr >> 16);
    b[4] = (UBYTE)(addr >> 24);
    /* The old block is finished: make it visible to the GPU now
     * (v3d_context_flush_for_dma flushes only the current block). */
    len = (ULONG)buf->used + 5;
    CachePreDMA(buf->start, &len, 0);
    if (ctx->chained_blocks++ < 8)
        ogles2_log("job: binning list block full at 0x%08lx + %ld: BRANCH to a new block at 0x%08lx (%ld bytes)",
                   (ULONG)buf->start, (LONG)buf->used, addr, (LONG)m.size);
    v3d_frame_defer_free(&v->frame, &m); /* freed after this frame's render */
    buf->start = (v3d_u8 *)m.hostptr;
    buf->used = 0;
    /* 256 bytes of slack for the control list reader's prefetch. */
    buf->capacity = (int)m.size - BRANCH_ROOM - 256;
    return v3d_buffer_claim_memory(buf, size);
}

/* ---- depth and stencil across a mid-frame flush (plan 7.2) ----
 * A job ends before its frame when the program reads pixels, copies them,
 * or calls glFlush/glFinish. Such a job stores depth and stencil (D24S8 as
 * one buffer) to memory of its own, and the next job of the frame loads
 * them instead of clearing early, as Mesa does for any surface outliving a
 * job. Only the GPU reads this memory, so its layout is the GPU's: UIF
 * without XOR, padded as Mesa pads a UIF level (v3d_setup_slices). At
 * aglSwapBuffers they are not kept (EGL: undefined after a swap). */
struct zs_save
{
    v3d_mem mem;
    ULONG base;            /* 4096-aligned within mem */
    ULONG w, h;            /* the framebuffer it was made for */
    ULONG height_in_ub;    /* padded height in UIF blocks */
    UBYTE buffer, format;  /* v3d_ZSTENCIL/v3d_Z, V3D_OUTPUT_IMAGE_FORMAT_* */
};

static struct zs_save *zs_get(struct OGLES2Context *ctx)
{
    struct zs_save *z = ctx->zs_save;
    V3DContext *v = ctx->v3d;
    ULONG cpp, ub_w, ub_h, pw, ph;
    if (!ctx->depth_bits && !ctx->stencil_bits)
        return 0;
    if (z && z->w == v->width && z->h == v->height)
        return z;
    if (z) {
        v3d_mem_free(ogles2_device(), &z->mem);
        FreeVec(z);
        ctx->zs_save = 0;
    }
    if (!(z = AllocVec(sizeof(*z), MEMF_ANY | MEMF_CLEAR)))
        return 0;
    cpp = ctx->depth_format == OGLES2_DEPTH_16 ? 2 : 4;
    ub_w = 2 * v3d_utile_width((int)cpp);
    ub_h = 2 * v3d_utile_height((int)cpp);
    pw = (v->width + 4 * ub_w - 1) / (4 * ub_w) * (4 * ub_w);
    ph = (v->height + ub_h - 1) / ub_h * ub_h;
    if (v3d_mem_alloc(ogles2_device(), &z->mem, pw * ph * cpp + 4096) < 0) {
        FreeVec(z);
        return 0;
    }
    z->base = ((ULONG)z->mem.hostptr + 4095) & ~4095UL;
    z->w = v->width;
    z->h = v->height;
    z->height_in_ub = ph / ub_h;
    z->buffer = ctx->depth_format == OGLES2_DEPTH_24 ? v3d_ZSTENCIL : v3d_Z;
    z->format = ctx->depth_format == OGLES2_DEPTH_16 ? V3D_OUTPUT_IMAGE_FORMAT_D16
              : ctx->depth_format == OGLES2_DEPTH_24 ? V3D_OUTPUT_IMAGE_FORMAT_D24S8
                                                     : V3D_OUTPUT_IMAGE_FORMAT_D32F;
    ctx->zs_save = z;
    return z;
}

void ogles2_job_free(struct OGLES2Context *ctx)
{
    struct zs_save *z = ctx->zs_save;
    if (z) {
        v3d_mem_free(ogles2_device(), &z->mem);
        FreeVec(z);
        ctx->zs_save = 0;
    }
    ctx->zs_saved = FALSE;
}

/* Starts the job's binning list if none is open. */
void ogles2_job_begin(struct OGLES2Context *ctx)
{
    V3DContext *v = ctx->v3d;
    struct OGLES2Target tg;
    UWORD w = v->width, h = v->height;
    BOOL bad = FALSE;
    v3d_u8 s;

    if (v->frame_active) {
        if (ctx->job_fb == ctx->draw_fb)
            return;
        /* Drawing into another framebuffer: the open job is rendered first
         * (jobs v1). The default framebuffer's depth and stencil are kept
         * for its next job, as at any mid-frame flush. */
        ogles2_job_flush(ctx, FALSE);
    }
    if (ctx->draw_fb) {
        if (ogles2_fbo_target(ctx, ctx->draw_fb, FALSE, &tg)) {
            w = tg.w;
            h = tg.h;
        } else {
            bad = TRUE; /* no memory for its tile pools: the job is dropped at the flush */
        }
    }
    ctx->job_fb = ctx->draw_fb;
    ctx->job_discard = 0;
    ctx->job_noload = ctx->draw_fb ? ogles2_fbo_take_discarded(ctx->draw_fb) : 0;
    v->frame.tilesX = (w + 63) / 64;
    v->frame.tilesY = (h + 63) / 64;
    s = v->build_slot;
    v3d_cl_reset(&v->tile_list_mem[s], &v->tile_list_buf[s]);
    v3d_cl_reset(&v->binning_mem[s], &v->binning_buf[s]);
    /* Back to the first block (a chained frame left buf->start in its last). */
    v->binning_buf[s].start = (v3d_u8 *)v->binning_mem[s].hostptr;
    v->binning_buf[s].capacity -= BRANCH_ROOM;
    v->binning_buf[s].grow = grow_binning;
    v->binning_buf[s].grow_data = ctx;
    v3d_cl_reset(&v->render_mem[s], &v->render_buf[s]);
    v3d_cl_reset(&v->state_mem[s], &v->state_buf[s]);
    g_v3d_cl_generation++;

    v->current_buf = &v->binning_buf[s];
    SetBuffer(v, &v->binning_buf[s]);
    /* As Mesa's v3d_start_binning (v3dx_draw.c). */
    NumberOfLayers(v, 1);
    TileBinningModeCfg(v, v3d_TILE_ALLOCATION_INITIAL_BLOCK_SIZE_64B,
                       v3d_TILE_ALLOCATION_BLOCK_SIZE_64B, 1, V3D_INTERNAL_BPP_32, FALSE, FALSE, w, h);
    DoCommand(v, v3d_OP_FLUSH_VCD_CACHE);
    OcclusionQueryCounter(v, 0);
    DoCommand(v, v3d_OP_START_TILE_BINNING);

    v->frame_active = TRUE;
    ctx->job_serial++;
    v->ez_state = V3D_EZ_UNDECIDED;
    v->first_ez_state = V3D_EZ_UNDECIDED;
    v->pending_clear_color = FALSE;
    v->pending_clear_depth = FALSE;
    v->frame_corrupted = bad;
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
static void build_render(struct OGLES2Context *ctx, struct zs_save *store_zs)
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
    /* Depth and stencil from the frame's previous job, unless this one
     * clears them anyway. */
    struct zs_save *load_zs = ctx->zs_saved && !v->pending_clear_depth ? zs_get(ctx) : 0;
    /* Mesa v3dx_rcl.c: the early Z/S clear only without a Z/S load or store;
     * otherwise the clear packets clear Z/S as well. */
    UBYTE early_zs_clear = !(load_zs || store_zs);
    if (store_zs && !ctx->zs_logged) {
        ctx->zs_logged = TRUE;
        ogles2_log("job: depth and stencil kept across a mid-frame flush (logged once)");
    }

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
    if (load_zs)
        LoadTileBufferGeneral(v, load_zs->buffer, V3D_MEMORY_FORMAT_UIF_NO_XOR, FALSE,
                              V3D_DECIMATE_MODE_SAMPLE_0, load_zs->format, FALSE, FALSE, FALSE,
                              load_zs->height_in_ub, 0, load_zs->base);
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
    if (store_zs)
        StoreTileBufferGeneral(v, store_zs->buffer, V3D_MEMORY_FORMAT_UIF_NO_XOR, FALSE,
                               V3D_DITHER_MODE_NONE, V3D_DECIMATE_MODE_SAMPLE_0, store_zs->format,
                               FALSE, FALSE, FALSE, store_zs->height_in_ub, 0, store_zs->base);
    /* Mesa v3dx_rcl.c, GFXH-1461/GFXH-1689: on 4.2 the per-store clear bit is
     * broken for Z/S, so clear with this packet, RTs bit set; Z/S bit clear
     * because the early Z/S clear does that. */
    if (clear_color || v->pending_clear_depth)
        ClearTileBuffers(v, !early_zs_clear, TRUE);
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
                               ez_off, depth_type,
                               /* Mesa: the early Z/S clear only without a Z/S load or
                                * store (with it, the stored depth was the clear value,
                                * A1200 run ogles2-m4-1); otherwise the dummy tile and
                                * the end-of-tile clear below clear the tile buffer */
                               early_zs_clear);
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
    ClearTileBuffers(v, !early_zs_clear, TRUE);
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

/* TILE_RENDERING_MODE_CFG (Clear Colors Part3), render target 0: only the
 * UIF padding, as raw little-endian bytes. Mesa (v3dx_rcl.c) sends it when
 * the image's padded height exceeds the job's by 15 UIF blocks or more. */
static void clear_colors_part3(V3DContext *v, ULONG pad)
{
    UBYTE *b = v3d_buffer_claim_memory(v->current_buf, 9);
    if (!b)
        return;
    b[0] = v3d_OP_TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART3;
    b[1] = 5;                       /* sub-id 5, render target 0 */
    b[2] = b[3] = 0;                /* clear colour, high 16 bits */
    b[4] = b[5] = 0;                /* raster stride or image height */
    b[6] = (UBYTE)pad;              /* bits 40..52: the padded height */
    b[7] = (UBYTE)((pad >> 8) & 0x1f);
    b[8] = 0;
}

/* The render list of a framebuffer object's job (Mesa's v3dx_rcl.c): colour
 * loaded unless cleared, depth and stencil loaded unless cleared, all
 * stored, in the images' own layouts; no Y flip. */
static void build_render_fbo(struct OGLES2Context *ctx, const struct OGLES2Target *tg, ULONG ta)
{
    V3DContext *v = ctx->v3d;
    v3d_u8 s = v->build_slot;
    UWORD width = tg->w, height = tg->h;
    ULONG tilesX = (width + 63) / 64, tilesY = (height + 63) / 64;
    ULONG start, end, sw = 1, sh = 1, fw, fh, sx, sy, argb, abgr;
    UBYTE clear_color = v->pending_clear_color, clear_zs = v->pending_clear_depth;
    UBYTE ez = v->first_ez_state;
    UBYTE ez_off = (ez == V3D_EZ_DISABLED) || (width & 1) || (height & 1); /* GFXH-1918 */
    /* EXT_discard_framebuffer: not stored by this job, not loaded from before it. */
    UBYTE no_store = ctx->job_discard, no_load = ctx->job_noload;
    BOOL store_c = tg->has_color && !(no_store & OGLES2_DISCARD_COLOR);
    BOOL store_z = tg->has_z && !(no_store & OGLES2_DISCARD_DEPTH &&
                                  (!tg->packed_zs || (no_store & OGLES2_DISCARD_STENCIL) || !ctx->fb_stencil_bits));
    BOOL store_s = tg->has_s && !(no_store & OGLES2_DISCARD_STENCIL);
    /* Mesa: the early Z/S clear only without a Z/S load or store. */
    UBYTE early_zs_clear = !(tg->has_z || tg->has_s);
    const struct OGLES2Surface *c = &tg->color, *z = &tg->z, *st = &tg->s;

    v->current_buf = &v->binning_buf[s];
    SetBuffer(v, &v->binning_buf[s]);
    if (ctx->probe & OGLES2_PROBE_SEMAPHORE)
        DoCommand(v, v3d_OP_INCREMENT_SEMAPHORE);
    DoCommand(v, v3d_OP_FLUSH);

    /* Generic tile list. */
    v->current_buf = &v->tile_list_buf[s];
    SetBuffer(v, &v->tile_list_buf[s]);
    start = CurrentBufferAddress(v);
    DoCommand(v, v3d_OP_TILE_COORDINATES_IMPLICIT);
    if (tg->has_color && !clear_color && !(no_load & OGLES2_DISCARD_COLOR))
        LoadTileBufferGeneral(v, v3d_RENDER_TARGET_0, c->mem_format, FALSE, V3D_DECIMATE_MODE_SAMPLE_0,
                              c->format, c->force_alpha1, FALSE, FALSE, c->ub, 0, c->addr);
    if (!clear_zs) {
        if (tg->has_z && !((no_load & OGLES2_DISCARD_DEPTH) &&
                           (!tg->packed_zs || (no_load & OGLES2_DISCARD_STENCIL) || !ctx->fb_stencil_bits)))
            LoadTileBufferGeneral(v, tg->packed_zs ? v3d_ZSTENCIL : v3d_Z, z->mem_format, FALSE,
                                  V3D_DECIMATE_MODE_SAMPLE_0, z->format, FALSE, FALSE, FALSE, z->ub, 0, z->addr);
        if (tg->has_s && !(no_load & OGLES2_DISCARD_STENCIL))
            LoadTileBufferGeneral(v, v3d_STENCIL, st->mem_format, FALSE, V3D_DECIMATE_MODE_SAMPLE_0,
                                  st->format, FALSE, FALSE, FALSE, st->ub, 0, st->addr);
    }
    DoCommand(v, v3d_OP_END_OF_LOADS);
    PrimListFormat(v, v3d_LIST_TRIANGLES, FALSE);
    SetInstanceid(v, 0);
    BranchToImplicitTileList(v, 0);
    if (store_c)
        StoreTileBufferGeneral(v, v3d_RENDER_TARGET_0, c->mem_format, FALSE, V3D_DITHER_MODE_NONE,
                               V3D_DECIMATE_MODE_SAMPLE_0, c->format, FALSE, FALSE, FALSE, c->ub, 0, c->addr);
    if (store_z)
        StoreTileBufferGeneral(v, tg->packed_zs ? v3d_ZSTENCIL : v3d_Z, z->mem_format, FALSE,
                               V3D_DITHER_MODE_NONE, V3D_DECIMATE_MODE_SAMPLE_0, z->format, FALSE, FALSE,
                               FALSE, z->ub, 0, z->addr);
    if (store_s)
        StoreTileBufferGeneral(v, v3d_STENCIL, st->mem_format, FALSE, V3D_DITHER_MODE_NONE,
                               V3D_DECIMATE_MODE_SAMPLE_0, st->format, FALSE, FALSE, FALSE, st->ub, 0, st->addr);
    if (!store_c && !store_z && !store_s) /* Mesa: a tile stores something */
        StoreTileBufferGeneral(v, v3d_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (clear_color || clear_zs)
        ClearTileBuffers(v, !early_zs_clear, TRUE);
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
                               ez_off, tg->depth_type, early_zs_clear);
    argb = v->clear_color_value;
    abgr = (argb & 0xFF00FF00UL) | ((argb & 0x00FF0000UL) >> 16) | ((argb & 0x000000FFUL) << 16);
    TileRenderingModeCFGClearColorsPart1(v, 0, abgr);
    if (tg->has_color && c->ub) {
        ULONG ubh = 2 * v3d_utile_height(c->format == V3D_OUTPUT_IMAGE_FORMAT_RGBA8 ? 4 : 2);
        if (c->ub - (height + ubh - 1) / ubh >= 15)
            clear_colors_part3(v, c->ub);
    }
    TileRenderingModeCFGColor(v, V3D_INTERNAL_BPP_32, V3D_INTERNAL_TYPE_8, 0);
    TileRenderingModeCFGZSClearValues(v, clear_zs ? ctx->job_clear_stencil : 0,
                                      clear_zs ? v->clear_depth_value : 1.0f);
    TileListInitialBlockSize(v, v3d_TILE_ALLOCATION_BLOCK_SIZE_64B, TRUE);

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
    Multicore_Rendering_Tile_List_Set_Base(v, ta, 0);
    MulticoreRenderingSupertileCFG(v, sw, sh, fw, fh, 1, FALSE, FALSE, tilesY, tilesX);

    TileCoordinates(v, 0, 0);
    DoCommand(v, v3d_OP_END_OF_LOADS);
    StoreTileBufferGeneral(v, v3d_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    ClearTileBuffers(v, !early_zs_clear, TRUE);
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

/* The colour image around the GPU's write (the CPU reads it back later:
 * texture.c's sync, glReadPixels): cleaned before, invalidated after. Depth
 * and stencil images are never read by the CPU. */
static void target_dma(const struct OGLES2Target *tg, BOOL after)
{
    ULONG len;
    if (!tg->has_color)
        return;
    len = tg->color.bytes;
    if (after)
        CachePostDMA((APTR)tg->color.addr, &len, 0);
    else
        CachePreDMA((APTR)tg->color.addr, &len, 0);
}

static BOOL any_overflow(V3DContext *v)
{
    v3d_u8 s = v->build_slot;
    return v->binning_buf[s].overflowed || v->state_buf[s].overflowed ||
           v->render_buf[s].overflowed || v->tile_list_buf[s].overflowed;
}

/* Renders the open job into the default framebuffer and waits for it.
 * Returns FALSE if the frame was dropped (the reason is logged). */
BOOL ogles2_job_flush(struct OGLES2Context *ctx, BOOL frame_end)
{
    V3DContext *v = ctx->v3d;
    V3DDevice *dev = ogles2_device();
    struct SignalSemaphore *gpu = ogles2_device_gpu_lock();
    v3d_u8 s = v->build_slot;
    v3d_wait_result wb = v3d_wait_result_success, wr = v3d_wait_result_success;
    const char *drop = 0;
    struct OGLES2Framebuffer *fb = ctx->job_fb;
    struct OGLES2Target tg;
    ULONG ta = (ULONG)v->tile_alloc_mem.hostptr, ta_size = (ULONG)v->tile_alloc_mem.size;
    ULONG ts = (ULONG)v->tile_state_mem.hostptr;

    struct zs_save *store_zs = 0;

    if (!v->frame_active) {
        if (frame_end)
            ctx->zs_saved = FALSE;
        return TRUE;
    }
    if (!fb && !frame_end && v->draw_state_configured &&
        !(ctx->job_discard & (OGLES2_DISCARD_DEPTH | OGLES2_DISCARD_STENCIL)))
        store_zs = zs_get(ctx); /* NULL without depth and stencil, or memory */
    if (v->frame_corrupted) {
        drop = fb ? "no memory for the framebuffer object's tile pools, or a draw's uniform stream went stale"
                  : "a draw's uniform stream went stale (state buffer grew mid-draw)";
        goto out;
    }
    if (fb) {
        if (!ogles2_fbo_target(ctx, fb, TRUE, &tg)) {
            drop = "the framebuffer object's images have no GPU memory";
            goto out;
        }
        if (tg.ta_addr) {
            ta = tg.ta_addr;
            ta_size = tg.ta_size;
            ts = tg.ts_addr;
        }
    }
    if (!v3d_gpu_lock(gpu, GPU_LOCK_TIMEOUT_MS)) {
        drop = "the GPU stayed busy (another program holds the submission lock)";
        goto out;
    }
    if (!fb && !ogles2_display_lock(&ctx->display)) {
        v3d_gpu_unlock(gpu);
        drop = "the render bitmap could not be locked";
        goto out;
    }

    if (fb)
        build_render_fbo(ctx, &tg, ta);
    else
        build_render(ctx, store_zs);
    if (any_overflow(v)) {
        if (!fb)
            ogles2_display_unlock(&ctx->display);
        v3d_gpu_unlock(gpu);
        drop = "a control list overflowed";
        goto out;
    }
    if (fb)
        target_dma(&tg, FALSE);
    else
        ogles2_display_dma(&ctx->display, FALSE);

    v3d_context_flush_for_dma(dev, v);
    v3d_invalidate_caches();
    if (ctx->probe & OGLES2_PROBE_SEMAPHORE) {
        /* Probe S: both lists go to the GPU at once; the render list waits
         * for the binner's semaphore. Out-of-memory service stays in the
         * binning wait. */
        v3d_u8 last = v3d_submit_binning(dev, &v->frame, (v3d_address)(ULONG)v->binning_mem[s].hostptr,
                                         (v3d_address)((ULONG)v->binning_buf[s].start +
                                                       v->binning_buf[s].used),
                                         (v3d_address)ta, (v3d_u32)ta_size, (v3d_address)ts);
        v3d_u8 frame_no = v3d_submit_render((v3d_address)(ULONG)v->render_buf[s].start,
                                            (v3d_address)((ULONG)v->render_buf[s].start +
                                                          v->render_buf[s].used));
        wb = v3d_wait_binning_timeout(dev, &v->frame, last);
        v3d_frame_render_submitted(&v->frame);
        wr = v3d_wait_render_timeout(frame_no);
        goto waited;
    }
    {
        v3d_u8 last = v3d_submit_binning(dev, &v->frame, (v3d_address)(ULONG)v->binning_mem[s].hostptr,
                                         (v3d_address)((ULONG)v->binning_buf[s].start +
                                                       v->binning_buf[s].used),
                                         (v3d_address)ta, (v3d_u32)ta_size, (v3d_address)ts);
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
    /* A job the GPU did not finish leaves it busy or stopped: reset it
     * before anyone submits again (plan 9 M4). */
    if (wb != v3d_wait_result_success || wr != v3d_wait_result_success)
        ogles2_device_reset(wb != v3d_wait_result_success ? "a binning job it did not finish"
                                                          : "a render job it did not finish");
    if (fb) {
        target_dma(&tg, TRUE);
    } else {
        ogles2_display_dma(&ctx->display, TRUE);
        ogles2_display_unlock(&ctx->display);
    }
    v3d_gpu_unlock(gpu);

    if (wb != v3d_wait_result_success) {
        drop = wb == v3d_wait_result_timed_out ? "binning timed out" : "binning failed";
        /* Where the control list executor stopped (raw little-endian words). */
        ogles2_log("job: binning stopped: CT0CA 0x%08lx CT0EA 0x%08lx CT0CS 0x%08lx; list 0x%08lx..0x%08lx "
                   "(last block 0x%08lx), %ld spill blocks",
                   (ULONG)LE32(V3D_CT0CA), (ULONG)LE32(V3D_CT0EA), (ULONG)LE32(V3D_CT0CS),
                   (ULONG)v->binning_mem[s].hostptr,
                   (ULONG)v->binning_buf[s].start + v->binning_buf[s].used, (ULONG)v->binning_buf[s].start,
                   (LONG)v->frame.job_spills);
    }
    else if (wr != v3d_wait_result_success)
        ogles2_log("job: render %s", (ULONG)(wr == v3d_wait_result_timed_out ? "timed out" : "failed"));
    else
        v3d_frame_retire(dev, &v->frame); /* only after a successful wait */
    ctx->frames++;
    v->build_slot = 1 - v->build_slot;
    g_v3d_cl_generation++;

out:
    v->frame_active = FALSE;
    if (fb) {
        /* The default framebuffer's saved depth and stencil stay for its
         * next job, unless the frame ends here. */
        if (frame_end)
            ctx->zs_saved = FALSE;
        if (!drop && wr == v3d_wait_result_success)
            ogles2_fbo_rendered(ctx, fb);
    } else {
        /* The next job loads depth and stencil only if this one stored them
         * and rendered (a frame end stores nothing). */
        ctx->zs_saved = !drop && wr == v3d_wait_result_success && store_zs;
    }
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
