#ifndef OGLES2_INTERNAL_H
#define OGLES2_INTERNAL_H
/*
 * ogles2.library internals shared by the library's own files.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <graphics/rastport.h>
#include <utility/tagitem.h>
#include "ogles2_api.h" /* generated: dispatch struct and every ogles2_* prototype */
#include <GLES2/gl2ext.h>
#include <libraries/ogles2.h> /* the extensions' enums gl2ext.h lacks */

/* 1.1: glDrawRangeElements appended to the dispatch table. 1.2: the
 * extension entries of AmigaOS 4's ogles2 (program binaries, glPolygonMode,
 * glProvokingVertex, glGetBufferParameterivOES). 1.3: M7's extensions
 * (docs/M7_EXTENSIONS_PLAN.md: debug markers, clip control, draw buffers).
 * glGetString's GL_VERSION (context.c) names the same revision. */
#define OGLES2_LIB_VERSION  1
#define OGLES2_LIB_REVISION 3
#define OGLES2_LIB_DATE     "13.10.2026"

struct OGLES2Library
{
    struct Library lib;
    BPTR segList;
    struct SignalSemaphore lock; /* library-wide state: context list, log */
    struct SignalSemaphore compiler_lock; /* starting the shader compiler (compiler.c) */
    ULONG contexts;                       /* with a GPU, in every program */
    /* Under `lock`: */
    ULONG serial;                         /* the last job serial handed out (job.c): unique across contexts */
    struct OGLES2Context *pend_ctx;       /* a context whose render may still run (job.c), */
    struct Task *pend_task;               /* the task that submitted it, */
    UBYTE pend_frame;                     /* and its v3d_submit_render count */
};

/* Objects shared between contexts (OGLES2_CCT_SHARE_WITH; GLES 2.0 appendix
 * C, plan 13 M6): the name tables of textures, buffers, shaders and programs
 * (with the images their programs run) and renderbuffers. Every context is
 * in a group (alone without the tag). Framebuffers, the default textures
 * (name 0) and every binding stay per context. A deleted object is detached
 * from every member (GL would keep it while another context has it bound).
 * The members may be used from several tasks: every call into a member of
 * a group of two or more runs under the group's semaphore (share.c). */
struct OGLES2Texture;
struct OGLES2Buffer;
struct OGLES2Renderbuffer;
struct OGLES2Image;
/* A name past OGLES2_NEAR_NAMES and its table slot (share.c). */
struct OGLES2FarName
{
    struct OGLES2FarName *next;
    GLuint name, slot;
};

struct OGLES2Share
{
    ULONG members;                    /* contexts in the group */
    struct OGLES2Context *first;      /* the members, through share_next */
    struct OGLES2Texture **tex;       /* texture.c: by name */
    GLuint ntex;
    struct OGLES2Buffer **buf;        /* buffer.c */
    GLuint nbuf;
    void **objects;                   /* program.c: shaders and programs, by name (0 never used) */
    GLuint nobjects;
    struct OGLES2Image *images;       /* program.c: the program cache, by hash (plan 7.8) */
    struct OGLES2Renderbuffer **rbs;  /* fbo.c */
    GLuint nrbs;
    struct OGLES2FarName *far_tex, *far_buf, *far_rb; /* their tables' names past OGLES2_NEAR_NAMES */
    struct SignalSemaphore lock;      /* every call into a member, while there are two or more */
};

/* A GL context. Opaque to clients (struct OGLES2Context in the headers). */
#define OGLES2_CONTEXT_MAGIC 0x474c4532UL /* "GLE2" */
#define OGLES2_MAX_VERTEX_ATTRIBS 16
/* Texture units are binding points: a program may use units 0-31 across its
 * stages (GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS), but a stage has at most 16
 * samplers, its texture indices 0-15 (GL_MAX_TEXTURE_IMAGE_UNITS, the
 * TMU_CONFIG recipes' index). M7 step 2; plan 7.6. */
#define OGLES2_MAX_TEXTURE_UNITS  32
#define OGLES2_MAX_STAGE_SAMPLERS 16
#define OGLES2_MAX_VIEWPORT 4096 /* V3D_MAX_FRAMEBUFFER_SIZE on 4.2; GL_MAX_VIEWPORT_DIMS */

/* glEnable capabilities, bits of OGLES2State.caps */
#define OGLES2_CAP_CULL_FACE                (1UL << 0)
#define OGLES2_CAP_BLEND                    (1UL << 1)
#define OGLES2_CAP_DITHER                   (1UL << 2)
#define OGLES2_CAP_STENCIL_TEST             (1UL << 3)
#define OGLES2_CAP_DEPTH_TEST               (1UL << 4)
#define OGLES2_CAP_SCISSOR_TEST             (1UL << 5)
#define OGLES2_CAP_POLYGON_OFFSET_FILL      (1UL << 6)
#define OGLES2_CAP_SAMPLE_ALPHA_TO_COVERAGE (1UL << 7)
#define OGLES2_CAP_SAMPLE_COVERAGE          (1UL << 8)

/* Fixed-function state (state.c), GLES 2.0 defaults set by ogles2_state_init. */
struct OGLES2State
{
    ULONG caps;                     /* OGLES2_CAP_* bits (glEnable) */
    GLboolean depth_mask;
    GLboolean color_mask[4];
    GLboolean sample_coverage_invert;
    GLfloat clear_color[4];
    GLfloat clear_depth;
    GLfloat line_width;
    GLfloat depth_range[2];         /* near, far */
    GLenum clip_origin, clip_depth; /* glClipControlEXT (M7): GL_LOWER_LEFT_EXT, GL_NEGATIVE_ONE_TO_ONE_EXT */
    GLfloat polygon_offset_factor, polygon_offset_units;
    GLfloat sample_coverage_value;
    GLfloat attrib[OGLES2_MAX_VERTEX_ATTRIBS][4]; /* current generic attributes */
    GLenum generate_mipmap_hint;    /* glHint */
    GLenum derivative_hint;         /* glHint, OES_standard_derivatives */
    GLenum polygon_mode;            /* glPolygonMode (AmigaOS 4): GL_FILL, GL_LINE, GL_POINT */
    GLenum provoking_vertex;        /* glProvokingVertex (AmigaOS 4) */
    GLenum cull_face_mode, front_face, depth_func;
    GLenum blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;
    GLenum blend_eq_rgb, blend_eq_alpha;
    GLfloat blend_color[4];
    /* stencil, [0] front and [1] back */
    GLenum stencil_func[2], stencil_fail[2], stencil_zfail[2], stencil_zpass[2];
    GLint stencil_ref[2];
    GLuint stencil_value_mask[2], stencil_write_mask[2];
    GLint clear_stencil;
};

struct OGLES2Buffer; /* buffer.c */

/* glVertexAttribPointer state, one per generic attribute. */
struct OGLES2Array
{
    GLboolean enabled, normalized;
    GLint size;          /* 1..4 components */
    GLenum type;
    GLsizei stride;      /* as given; 0 = tightly packed */
    const void *pointer; /* with a buffer: the offset into it */
    struct OGLES2Buffer *buffer; /* the GL_ARRAY_BUFFER binding when it was set; NULL: client memory */
};

/* ---- program images (program.c, v3dp.c) ----
 * What a linked program runs: compiled shaders plus what the driver must
 * know to draw with them, Mesa's prog_data in short. Loaded from .v3dp files
 * (plan section 8.3, v3dp/FORMAT.md) by v3dp.c; one is built in (PiStorm3D's
 * smooth-shaded shaders, builtin_shaders.h), for clears drawn as rectangles. */

/* Uniform recipe types: Mesa's enum quniform_contents at the validated commit
 * (v3dp/FORMAT.md), those the runtime implements; the built-in image's own
 * from 0x100. */
#define OGLES2_U_CONSTANT          0
#define OGLES2_U_UNIFORM           1
#define OGLES2_U_VIEWPORT_X_SCALE  2
#define OGLES2_U_VIEWPORT_Y_SCALE  3
#define OGLES2_U_VIEWPORT_Z_OFFSET 4
#define OGLES2_U_VIEWPORT_Z_SCALE  5
#define OGLES2_U_TMU_CONFIG_P0     6  /* data: texture index << 24 | config bits */
#define OGLES2_U_TMU_CONFIG_P1     7
#define OGLES2_U_UBO_ADDR          16 /* data: unit << 24 | byte offset; unit 0 only */
#define OGLES2_U_LINE_WIDTH        26
#define OGLES2_U_AA_LINE_WIDTH     27
#define OGLES2_U_SPILL_OFFSET      31 /* the context's spill area (register spilling, draw.c) */
#define OGLES2_U_SPILL_SIZE_PER_THREAD 32
#define OGLES2_U_BLEND_CONSTANT_R  42 /* to _A, 45 */
#define OGLES2_U_CLIP_Z_COEFF      46 /* Mesa patch 0013 (EXT_clip_control): z lerped towards w */
#define OGLES2_U_PISTORM_VP_WIDTH  0x100 /* PiStorm3D's scale_p: viewport width in pixels */
#define OGLES2_U_PISTORM_VP_HEIGHT 0x101 /* PiStorm3D's scale_p_y */

/* Mesa's gl_state_index values at the validated commit, for STAGE_PARAMS. */
#define OGLES2_STATE_DEPTH_RANGE        39
#define OGLES2_STATE_FB_WPOS_Y_TRANSFORM 63
#define OGLES2_STATE_FB_PNTC_Y_TRANSFORM 64

#define OGLES2_VARIANT_VS 0 /* .v3dp VARIANT stages */
#define OGLES2_VARIANT_CS 1 /* binning (coordinate) shader */
#define OGLES2_VARIANT_FS 4

/* One compiled shader (a .v3dp VARIANT): its key fields the runtime selects
 * on, its prog_data, its code and its uniform recipe. */
struct OGLES2Variant
{
    UBYTE stage;                  /* OGLES2_VARIANT_* */
    UBYTE threads, single_seg;
    /* vertex and binning */
    UBYTE per_vertex_point_size;  /* key */
    UBYTE separate_segments, vcm_cache_size, uses_vid, uses_iid, writes_psiz;
    UBYTE vattr_sizes[16];
    ULONG vpm_input_size, vpm_output_size;
    /* fragment */
    UBYTE is_points, is_lines;    /* key */
    UBYTE num_inputs, writes_z, writes_z_from_fep, disable_ez, uses_center_w;
    UBYTE uses_implicit_point_line_varyings, lock_scoreboard_on_first_thrsw, uses_pid;
    ULONG flat_shade_flags[3], noperspective_flags[3], centroid_flags[3];
    ULONG spill_size;
    /* code, little-endian QPU instructions; its offset in the image's code */
    const UBYTE *code;
    ULONG code_bytes, code_offset;
    UWORD nrecipe;
    const ULONG *rtype, *rdata;
    UBYTE uses_ubo;               /* the recipe has OGLES2_U_UBO_ADDR (v3dp.c; 0 in the built-in image) */
    /* v3dp.c, for draw.c's uniform streams (NULL in the built-in image):
     * each entry's constant buffer word (0 for the others), and the
     * entries that are not constant buffer words. */
    const UWORD *uidx;
    const UWORD *special;
    UWORD nspecial;
    UBYTE no_textures;            /* no TMU or UBO entries: its stream depends on no texture */
    UBYTE cbufs;                  /* fragment key: the render targets it writes (bit i: target i;
                                   * 0 in the built-in image: target 0) */
};

struct OGLES2ImageAttrib
{
    const char *name;
    GLenum type;          /* GL_FLOAT_VEC3, ... (glGetActiveAttrib) */
    UBYTE slot;           /* the slot of its first column read; 0xff: none read */
    UBYTE columns;        /* 1, or 2-4 for a matrix */
    UBYTE colmask;        /* the columns read, each in the next slot (bit k: column k); 0: from a
                           * compiler that did not say, every column from `slot` on */
};
/* Attribute names of a program: 16 locations, more names with aliasing
 * (GLES 2.0 2.10.4: two names bound to one location). */
#define OGLES2_MAX_IMAGE_ATTRIBS 64

/* Where glUniform* writes one uniform in one stage's constant buffer. */
struct OGLES2Place
{
    UBYTE stage;          /* 0 vertex, 1 fragment: the constant buffer */
    UBYTE elem_stride;    /* 32-bit words between array elements */
    UBYTE vec_stride;     /* 32-bit words between matrix columns */
    UBYTE int_as_float;   /* Mesa's uniform_int_float format */
    UWORD offset;         /* 32-bit words */
};

#define OGLES2_UNIFORM_SAMPLER 1
#define OGLES2_UNIFORM_BUILTIN 2
#define OGLES2_UNIFORM_HIDDEN  4

struct OGLES2ImageUniform
{
    const char *name;     /* arrays without [0] (Mesa's naming) */
    GLenum type;          /* of one element */
    UWORD array;          /* elements, 0 if not an array */
    UBYTE flags;          /* OGLES2_UNIFORM_* */
    UBYTE nplaces;
    const struct OGLES2Place *places;
    ULONG sampler[2];     /* texture index in the vertex and fragment stage, ~0: none */
};

/* A state variable (STAGE_PARAMS kind 2): four values the runtime computes. */
struct OGLES2StateVar
{
    UBYTE stage;          /* 0 vertex, 1 fragment */
    UWORD offset;
    UWORD state;          /* OGLES2_STATE_* */
};

struct v3d_mem;

struct OGLES2Image
{
    const char *name;     /* for the log */
    UBYTE hash[32];       /* the program hash (loaded images) */
    UWORD nvariants;
    const struct OGLES2Variant *variants;
    UBYTE nattribs;
    const struct OGLES2ImageAttrib *attribs;
    UWORD nuniforms;
    const struct OGLES2ImageUniform *uniforms;
    UWORD cbuf_words[2];                  /* constant buffers, vertex and fragment */
    const ULONG *cbuf_template[2];
    UWORD nstatevars;
    const struct OGLES2StateVar *statevars;
    ULONG code_total;                     /* bytes of code, all variants */
    BOOL clip_z_coeff;                    /* its vertex shaders read OGLES2_U_CLIP_Z_COEFF (compiled
                                             with Mesa patch 0013): ZERO_TO_ONE clips (draw.c) */
    struct v3d_mem *code_mem;             /* the code in GPU memory; NULL: built in */
    APTR blocks;                          /* v3dp.c's allocations, to free */
    UBYTE *binary;                        /* the .v3dp file it was loaded from (AllocVec'd; NULL:
                                             built in): OES_get_program_binary's binary */
    ULONG binary_len;
    /* The variants for samplers returning 32 bits (float and depth
     * textures): the program's .v3dw, loaded or compiled at the first draw
     * that needs them (program.c, ogles2_image_wide); only its fragment
     * variants are used. The sources it is compiled from (NULL: a program
     * from glProgramBinaryOES, which can only find a .v3dw). */
    struct OGLES2Image *wide;
    BOOL wide_tried;
    /* The alpha-to-coverage variants (Mesa's fragment key msaa and
     * sample_alpha_to_coverage): the program's .v3da, the same way, for a
     * draw into a 4x MSAA default framebuffer with GL_SAMPLE_ALPHA_TO_COVERAGE. */
    struct OGLES2Image *a2c;
    BOOL a2c_tried;
    char *vs_src, *fs_src;
    struct OGLES2Image *next;             /* the context's program cache (program.c) */
};

/* GL shader and program objects share one name space (GLES 2.0 section 2.10). */
#define OGLES2_OBJ_SHADER  1
#define OGLES2_OBJ_PROGRAM 2

struct OGLES2Shader
{
    UBYTE kind;           /* OGLES2_OBJ_SHADER */
    BOOL delete_pending;
    GLuint name;
    GLenum type;          /* GL_VERTEX_SHADER, GL_FRAGMENT_SHADER */
    char *source;         /* AllocVec'd, NUL-terminated */
    char *info_log;       /* AllocVec'd or NULL: the compiler's messages (plan 8.4) */
    GLboolean compiled;
    UWORD attached;       /* programs it is attached to */
};

#define OGLES2_MAX_BINDINGS 64 /* glBindAttribLocation names a program keeps (dEQP binds 32) */
struct OGLES2Program
{
    UBYTE kind;           /* OGLES2_OBJ_PROGRAM */
    BOOL delete_pending;
    GLuint name;
    struct OGLES2Shader *vs, *fs;
    GLboolean linked;
    const struct OGLES2Image *image;  /* after a successful link */
    UBYTE attrib_index[OGLES2_MAX_IMAGE_ATTRIBS]; /* image attribute i <- generic attribute */
    ULONG *cbuf[2];                   /* constant buffers, vertex and fragment */
    ULONG *cbuf_le[2];                /* the same in the GPU's byte order, for the draws'
                                         uniform streams: written with ogles2_cbuf_set */
    UWORD nlocations;                 /* uniform locations: one per array element */
    UWORD *loc_uniform, *loc_element; /* location -> uniform, element */
    UBYTE *sampler_unit;              /* per location: a sampler's (an array element's)
                                         texture unit (glUniform1i) */
    UWORD *uniform_loc;               /* per uniform of the image: its first location,
                                         0xFFFF for a hidden one */
    char bind_name[OGLES2_MAX_BINDINGS][32]; /* glBindAttribLocation, applied at link */
    GLuint bind_index[OGLES2_MAX_BINDINGS];
    UBYTE nbindings;
    char *info_log;                   /* AllocVec'd or NULL */
    /* draw.c: its last GL shader state record without the addresses that
     * change per draw, for the variants and code it was made from. */
    APTR rec_tmpl;                    /* AllocVec'd or NULL; freed at unlink */
    const struct OGLES2Variant *rec_var[3];
    ULONG rec_code[3];
    BOOL rec_points;
    /* draw.c: the variants and code addresses for the last primitive mode
     * drawn, and the image's sampler uniforms (0xff: not listed yet). */
    BOOL var_ok;
    GLenum var_mode, var_fsmode;      /* the draw's primitive, the fragment shader's (glPolygonMode) */
    const struct OGLES2Image *var_fim; /* the fragment shader's image: the program's, or its .v3dw */
    UBYTE var_nrt;                    /* the render targets drawn into (EXT_draw_buffers) */
    const struct OGLES2Variant *var[3];
    ULONG var_code[3];
    UBYTE nsamplers;
    UWORD samplers[16];
    /* draw.c: the coordinate and vertex shaders' last uniform streams,
     * reused while the vertex constant buffer and the state they read are
     * the same (cbuf_ver[0], the context's ustate_ver), in the same
     * context's job (a shared program draws in several). */
    ULONG cbuf_ver[2];                /* bumped by every change to cbuf[k] */
    const struct OGLES2Variant *us_var[2];
    ULONG us_gen[2], us_cbuf_ver[2], us_state_ver[2], us_addr[2];
    struct OGLES2Context *us_ctx[2];
    UBYTE nslots;                     /* vertex input slots (0xff: not mapped yet) */
    UBYTE slot_attrib[16];            /* the generic attribute of each (0xff: none) */
};

struct OGLES2Context;
/* program.c: one word of a constant buffer and its little-endian copy. */
void ogles2_cbuf_set(struct OGLES2Program *p, int stage, ULONG at, ULONG w);
/* program.c: a program on the built-in image, outside the name table
 * (draw.c's clears); its storage freed with ogles2_program_builtin_free. */
BOOL ogles2_program_builtin(struct OGLES2Program *p);
BOOL ogles2_program_image(struct OGLES2Program *p, const struct OGLES2Image *im);
void ogles2_program_builtin_free(struct OGLES2Program *p);

/* v3dp.c: a .v3dp file's image, or NULL with the reason in why[]. */
struct OGLES2Image *ogles2_v3dp_load(const UBYTE *file, ULONG len, char *why, ULONG whylen);
void ogles2_image_free(struct OGLES2Image *im, struct OGLES2Context *ctx);
/* The image's variant for `stage` and a draw of primitive `mode` (GL_POINTS ...). */
const struct OGLES2Variant *ogles2_image_variant(const struct OGLES2Image *im, UBYTE stage, UBYTE cbufs,
                                                 GLenum mode);
/* program.c: the image's 32-bit-sampler variants (im->wide), found or
 * compiled the first time; NULL if there are none (logged once). */
const struct OGLES2Image *ogles2_image_wide(struct OGLES2Image *im);
/* program.c: its alpha-to-coverage variants (im->a2c), the same way. */
const struct OGLES2Image *ogles2_image_a2c(struct OGLES2Image *im);

/* The default framebuffer's surface (display.c). */
struct OGLES2Display
{
    struct Screen *screen;          /* fullscreen: ours; window: the window's */
    struct Window *window;          /* window mode: the window we ClipBlit into */
    struct Window *input_window;    /* fullscreen: our backdrop window */
    struct BitMap *bitmap;          /* what the GPU renders into */
    struct RastPort rastport;       /* window mode: ClipBlit source over bitmap */
    UWORD width, height;
    BOOL own_screen, own_window, own_bitmap, vsync;
    int rb_swap;                    /* 1: BGRA32 target, 0: RGBA32 */
    APTR lock;                      /* cybergraphics bitmap lock, one frame long */
    ULONG bprow, base;              /* valid while locked */
    /* Vsync (display.c): the task raised from the wait for the refresh to
     * its frame's submission, and its own priority. */
    struct Task *boosted;
    BYTE boost_pri;
};

struct V3DContext; /* v3dcore */

/* ---- framebuffer objects (fbo.c) ----
 * One image a job's tile buffer loads from and stores to (Mesa's
 * load_general/store_general fields). */
struct OGLES2Surface
{
    ULONG addr;            /* the image's first byte */
    ULONG bytes;           /* its extent, for the CPU cache */
    UBYTE mem_format;      /* V3D_MEMORY_FORMAT_* (the level's own tiling) */
    UBYTE format;          /* V3D_OUTPUT_IMAGE_FORMAT_* */
    UBYTE force_alpha1;    /* load: an RGB format */
    ULONG ub;              /* UIF: padded height in UIF blocks; else 0 */
};

/* What a framebuffer object's job renders into (ogles2_fbo_target). */
struct OGLES2Texture;
/* EXT_draw_buffers (M7 step 5): colour attachments 0..3, each a render
 * target of the tile buffer. */
#define OGLES2_MAX_DRAW_BUFFERS 4

struct OGLES2Target
{
    UWORD w, h;
    UBYTE colors;          /* bit i: colour attachment i is drawn (a draw buffer) and has an image */
    UBYTE nrt;             /* the tile buffer's render targets: the highest drawn + 1 (1 with none) */
    UBYTE tile_w, tile_h;  /* the tile size nrt and bpp give (Mesa's v3d_choose_tile_size) */
    UBYTE bpp;             /* V3D_INTERNAL_BPP_*: the widest drawn target's (64 with a 16F one) */
    BOOL has_z, has_s, packed_zs; /* packed: one D24S8 image for both */
    struct OGLES2Surface color[OGLES2_MAX_DRAW_BUFFERS], z, s;
    UBYTE depth_type;      /* V3D_INTERNAL_TYPE_DEPTH* for the tile buffer */
    ULONG ta_addr, ta_size, ts_addr; /* own tile pools, 4096-aligned; 0: the context's fit */
};

struct OGLES2Context
{
    ULONG magic;
    /* At offset 4: the dispatch table's wrappers test it in assembly
     * (gen/gen_ogles2.py, share.c): the group's semaphore while it has two
     * members or more, else 0. */
    struct SignalSemaphore *share_lock;
    GLenum error;        /* first error since the last glGetError (sticky) */
    ULONG width, height; /* default framebuffer size; 0 without a display */
    struct OGLES2State state;
    struct OGLES2Display display;   /* zeroed for a state-only (test) context */
    struct V3DContext *v3d;         /* GPU pools and control lists; NULL state-only */
    UBYTE depth_bits, stencil_bits; /* of the default framebuffer */
    UBYTE depth_format;             /* OGLES2_DEPTH_*: the tile buffer's depth type */
    BOOL msaa;                      /* 4x MSAA on the default framebuffer (OGLES2_CCT_PISTORM_MSAA) */
    UBYTE job_clear_stencil;        /* the open job's stencil clear value (job.c) */
    GLfloat job_clear_rgba[4];      /* its colour clear value, for 16F targets (job.c) */
    UBYTE copy_logged;              /* glCopyTex* on the GPU: 1 from the default framebuffer, 2 from
                                     * an object, logged (copy.c) */
    GLint viewport[4];              /* x, y, width, height */
    GLint scissor[4];
    ULONG frames, frames_dropped;   /* statistics, logged at destroy */
    /* Timing (microseconds, ogles2_clock_us; logged at destroy): per job the
     * CPU's part (job begun to flush) and the waits for binning and render;
     * per frame the time between swaps and the presentation. */
    ULONG t_job_begin, t_last_swap;
    double sum_build, sum_bin, sum_render, sum_present, sum_frame;
    double sum_list, sum_pre, sum_post; /* the flush's own: render list, cache before, after */
    ULONG n_jobs, n_swaps;
    ULONG n_renames;                /* textures renamed (texture.c) */
    ULONG n_draws;                  /* draws issued (draw.c) */
    double sum_draw, sum_bufdata, sum_teximage; /* microseconds in draws, buffer data, texture images */
    double p_draw, p_bufdata, p_teximage;
    /* Probe P: microseconds in each stage of a draw (draw.c's DRAW_STAGES). */
    ULONG t_stage;
    double stage[8], p_stage[8];
    double n_uwords, n_vbytes, n_indices; /* probe P: uniform stream words, vertex bytes, indices copied */
    /* draw.c: the state the uniform recipes read besides the constant
     * buffers, as at the last draw, and its version. */
    ULONG ustate[7], ustate_ver;
    /* Bumped by every change to the state draw.c's emit_state reads (the
     * setters in state.c, glViewport and glScissor, glClear's override):
     * a draw with the same version as the last skips emit_state. */
    ULONG state_ver;
    ULONG p_draws;
    ULONG n_deletes;                /* glDelete* calls (probe T) */
    double p_frame, p_build, p_render, p_list, p_pre; /* the sums at the last probe P report */
    ULONG p_jobs, p_renames;
    ULONG probe;                    /* OGLES2_PROBE_*, from ENV:OGLES2/Probe (context.c) */
    ULONG probe_addr;               /* OGLES2_PROBE_ADDR's buffer (4 KB, the test's) */
    struct OGLES2Array array[OGLES2_MAX_VERTEX_ATTRIBS];
    struct OGLES2Share *share;      /* the objects (alone in the group without OGLES2_CCT_SHARE_WITH) */
    struct OGLES2Context *share_next; /* the group's next member */
    struct OGLES2Program *program;  /* glUseProgram */
    struct OGLES2Draw *draw;        /* draw.c's per-context state, NULL until the first draw */
    struct OGLES2TexState *tex;     /* texture.c's, NULL until first used */
    UBYTE unpack_alignment, pack_alignment; /* glPixelStorei */
    ULONG job_serial;               /* the open job's, unique across contexts (job.c); objects note the
                                     * last job that used them */
    struct OGLES2BufState *buf;     /* buffer.c's, NULL until first used */
    BOOL debug_shader_log;          /* OGLES2_CCT_DEBUG_SHADER_LOG: link logs describe the variants */
    ULONG chained_blocks;           /* binning-list blocks added (job.c); the first is logged */
    void *zs_save;                  /* job.c: memory for depth/stencil across a mid-frame flush */
    BOOL zs_saved;                  /* the next job of this frame loads depth/stencil from it */
    BOOL zs_logged;
    /* Framebuffer objects (fbo.c, plan 7.7). width/height/depth_bits/
     * stencil_bits/depth_format above are the default framebuffer's; the
     * fb_* fields describe the framebuffer drawn into now (ogles2_fb_update). */
    struct OGLES2FboState *fbo;     /* fbo.c's, NULL until first used */
    struct OGLES2Framebuffer *draw_fb; /* GL_FRAMEBUFFER binding; NULL: the default */
    struct OGLES2Framebuffer *job_fb;  /* what the open job renders into */
    BOOL job_msaa;                  /* the open job is 4x MSAA (the default framebuffer's, job.c) */
    UWORD fb_w, fb_h;
    BOOL fb_flip;                   /* the default framebuffer: drawn upside down (plan 7.11) */
    UBYTE fb_depth_bits, fb_stencil_bits, fb_depth_format;
    UBYTE fb_draw_mask;             /* EXT_draw_buffers: bit i, output i is drawn into an image */
    UBYTE fb_nrt;                   /* the tile buffer's render targets for it: 1 to 4 */
    GLenum default_draw_buffer;     /* glDrawBuffersEXT on the default framebuffer: GL_BACK, GL_NONE */
    /* M6 overlap (job.c): a render submitted and not waited for yet. The
     * submission lock went at the submission; the next submitter waits for
     * the render on the GPU (take_gpu) and, from another task, marks it
     * done; ogles2_job_finish does the rest. */
    BOOL pend;
    BOOL pend_done;                 /* another task saw it finish (under OGLES2Library.lock) */
    UBYTE pend_frame;               /* v3d_submit_render's count */
    ULONG pend_serial;              /* its job_serial */
    struct OGLES2Framebuffer *pend_fb; /* NULL: the default framebuffer */
    struct OGLES2Target pend_tg;
    UBYTE marker_depth;             /* EXT_debug_marker: groups pushed (debug.c) */
    UBYTE job_discard;              /* EXT_discard_framebuffer: OGLES2_DISCARD_* the open job does not store */
    UBYTE job_noload;               /* and those it does not load (discarded before it began) */
};

#define OGLES2_DISCARD_COLOR   1
#define OGLES2_DISCARD_DEPTH   2
#define OGLES2_DISCARD_STENCIL 4


/* EXT_blend_minmax (gl2ext.h) */
#ifndef GL_MIN_EXT
#define GL_MIN_EXT 0x8007
#define GL_MAX_EXT 0x8008
#endif

#define OGLES2_DEPTH_16  0
#define OGLES2_DEPTH_24  1 /* with 8 stencil bits */
#define OGLES2_DEPTH_32F 2

/* Hardware probes for the A1200 tests, one letter each in ENV:OGLES2/Probe.
 * Not for applications: they change what the library does. */
#define OGLES2_PROBE_STORE_DITHER 1UL /* D: dithered RGBA8 store */
#define OGLES2_PROBE_LOG_CLEAR   2UL  /* C: log the packed clear colour of each job */
/* The M1 hardware probes (plan section 9, M1): */
/* 4UL was F (GL_FIXED as the hardware's fixed type): it works and is the normal path now. */
#define OGLES2_PROBE_FLIP_Y     8UL  /* Y: colour stored and loaded with Flip Y */
#define OGLES2_PROBE_SEMAPHORE 16UL  /* S: render queued behind binning (semaphore packets) */
#define OGLES2_PROBE_ADDR      32UL  /* A: positions copied to ENV:OGLES2/ProbeAddr (hex) */
/* M6 diagnosis (a program that hangs or slows down): */
#define OGLES2_PROBE_TRACE     64UL  /* T: log every glDelete* call and each step of aglDestroyContext */
#define OGLES2_PROBE_PERIODIC 128UL  /* P: every 256 swaps, the timing, GPU memory and renames since the last */
#define OGLES2_PROBE_CPU_MIPMAP 256UL /* M: glGenerateMipmap on the CPU, not the TFU (texture.c) */
#define OGLES2_PROBE_CPU_UPLOAD 512UL /* U: texture uploads tiled by the CPU, not the TFU (texture.c) */
#define OGLES2_PROBE_CPU_COPY 1024UL /* K: glCopyTex* through the CPU, not drawn by the GPU (copy.c) */

void ogles2_state_init(struct OGLES2Context *ctx);
BOOL ogles2_state_dither(struct OGLES2Context *ctx); /* GL_DITHER enabled */

/* ---- display (display.c) ---- */
BOOL ogles2_display_libs_open(void);
void ogles2_display_libs_close(void);
BOOL ogles2_display_open(struct OGLES2Display *d, struct TagItem *tags, ULONG *why);
void ogles2_display_close(struct OGLES2Display *d);
BOOL ogles2_display_set_window(struct OGLES2Display *d, struct Window *w);
void ogles2_display_drop(struct OGLES2Display *d);
BOOL ogles2_display_set_bitmap(struct OGLES2Display *d, struct BitMap *bitmap);
/* Locked: the CPU cache around the GPU's write of the bitmap (before: clean;
 * after: invalidate), where the CPU reads it. */
void ogles2_display_dma(struct OGLES2Display *d, BOOL after);
BOOL ogles2_display_lock(struct OGLES2Display *d);
void ogles2_display_unlock(struct OGLES2Display *d);
void ogles2_display_present(struct OGLES2Display *d);
/* aglSetParams2's VSYNC. */
void ogles2_display_set_vsync(struct OGLES2Display *d, BOOL on);
/* Vsync: the wait for the display's refresh before the frame goes out
 * (raising the task's priority), and the end of it after the submission. */
void ogles2_display_wait_refresh(struct OGLES2Display *d);
void ogles2_display_wait_done(struct OGLES2Display *d);
/* glReadPixels: w x h pixels from (x, y) (GL's origin, the bottom-left),
 * clipped by the caller, as RGBA bytes, bottom row first, `stride` apart. */
void ogles2_display_read(struct OGLES2Display *d, UWORD x, UWORD y, UWORD w, UWORD h, UBYTE *dst,
                         ULONG stride);

/* ---- programs (program.c) and drawing (draw.c) ---- */
void *ogles2_object(struct OGLES2Context *ctx, GLuint name, UBYTE kind); /* NULL if not that kind */
void ogles2_objects_free(struct OGLES2Context *ctx); /* aglDestroyContext: programs, shaders, cache */
void ogles2_draw_free(struct OGLES2Context *ctx);    /* aglDestroyContext, after the last job */
const struct OGLES2Image *ogles2_builtin_image(void);
/* texture.c */
void ogles2_textures_free(struct OGLES2Context *ctx);
/* for fbo.c */
struct OGLES2Texture *ogles2_texture_get(struct OGLES2Context *ctx, GLuint name); /* NULL: none */
GLuint ogles2_texture_name(const struct OGLES2Texture *t);
GLenum ogles2_texture_target(const struct OGLES2Texture *t); /* 0 until first bound */
/* An attachment point refers to it, or no longer does (a deleted one that
 * was attached goes with its last attachment; `now`: at aglDestroyContext). */
void ogles2_texture_attach(struct OGLES2Texture *t);
void ogles2_texture_detach(struct OGLES2Context *ctx, struct OGLES2Texture *t, BOOL now);
/* The image of face k, level l: FALSE if not defined. */
BOOL ogles2_texture_image(const struct OGLES2Texture *t, int k, int l, UWORD *w, UWORD *h,
                          GLenum *format, GLenum *type);
/* Colour-renderable (plan 7.7): RGBA8, RGB565, RGBA4, RGB5_A1 storage; RGBA16F
 * (half-float RGBA, EXT_color_buffer_half_float). */
BOOL ogles2_texture_renderable(const struct OGLES2Texture *t, int k, int l);
/* That image is half-float: a 64-bit render target (16F in the tile buffer). */
BOOL ogles2_texture_half(const struct OGLES2Texture *t, int k, int l);
UWORD ogles2_float_to_half(GLfloat f); /* IEEE half precision, by the bits (texture.c) */
/* glCopyTex* on the GPU (copy.c): the default framebuffer's region tiled by
 * the TFU into a scratch texture (0: could not); unit 0's 2D texture swapped
 * for the copy's draw; a framebuffer's colour texture sampled level 0,
 * nearest and clamped for it, and its parameters put back. */
struct OGLES2Texture *ogles2_texture_scratch(struct OGLES2Context *ctx, UWORD w, UWORD h, ULONG src, ULONG stride,
                                             BOOL bgra);
struct OGLES2Texture *ogles2_texture_unit0(struct OGLES2Context *ctx, struct OGLES2Texture *t);
struct OGLES2TexParams
{
    GLenum min_filter, mag_filter, wrap_s, wrap_t;
    GLint base_level, max_level;
    GLfloat max_aniso, lod_bias;
};
void ogles2_texture_params_copy(struct OGLES2Texture *t, struct OGLES2TexParams *saved);
void ogles2_texture_params_restore(struct OGLES2Texture *t, const struct OGLES2TexParams *saved);
BOOL ogles2_copy_gpu(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, int level, GLint xoffset,
                     GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
BOOL ogles2_draw_copy(struct OGLES2Context *ctx, struct OGLES2Texture *src, const GLfloat *tc, GLint x, GLint y,
                      GLsizei w, GLsizei h);
struct OGLES2Framebuffer;
struct OGLES2Framebuffer *ogles2_fbo_copy_target(struct OGLES2Context *ctx, struct OGLES2Texture *t, int face,
                                                 int level);
BOOL ogles2_fbo_color_image(struct OGLES2Context *ctx, struct OGLES2Texture **t, int *face, int *level);
/* The image as a render target, its GPU storage made first; FALSE: none. */
BOOL ogles2_texture_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, int l,
                            struct OGLES2Surface *s);
void ogles2_texture_rendered(struct OGLES2Texture *t, int k, int l); /* the GPU's copy is newer */
/* OES_depth_texture: level l is a depth image (bits 16 or 24; stencil: DEPTH_STENCIL_OES). */
BOOL ogles2_texture_depth(const struct OGLES2Texture *t, int l, UBYTE *bits, BOOL *stencil);
/* That image as a job's depth/stencil surface, its GPU storage made first; FALSE: none. */
BOOL ogles2_texture_zs_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int l,
                               struct OGLES2Surface *s);
/* The texture kinds a sampler reads (its uniform's type). */
#define OGLES2_TEX_2D   0
#define OGLES2_TEX_CUBE 1
#define OGLES2_TEX_RECT 2 /* ARB_texture_rectangle */
static inline int ogles2_sampler_kind(GLenum type)
{
    return type == GL_SAMPLER_CUBE ? OGLES2_TEX_CUBE : type == GL_SAMPLER_2D_RECT ? OGLES2_TEX_RECT : OGLES2_TEX_2D;
}
/* draw.c: the texture a sampler of that kind on `unit` reads returns 32 bits. */
BOOL ogles2_texture_wide(struct OGLES2Context *ctx, int unit, int kind);
extern ULONG ogles2_wide_seen; /* such a texture was ever given an image */
/* A box of the image as RGBA8 rows (row 0 = GL's bottom), from the GPU's copy. */
BOOL ogles2_texture_read(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h,
                         UBYTE *rgba, ULONG stride);
/* A renderbuffer's colour storage: a texture of its own, in no name table. */
struct OGLES2Texture *ogles2_texture_hidden(struct OGLES2Context *ctx, UWORD w, UWORD h,
                                            GLenum format, GLenum type);
void ogles2_texture_hidden_free(struct OGLES2Context *ctx, struct OGLES2Texture *t);
/* fbo.c */
void ogles2_fb_update(struct OGLES2Context *ctx);       /* the fb_* fields from the binding */
GLenum ogles2_fb_status(struct OGLES2Context *ctx);     /* of the draw framebuffer */
BOOL ogles2_fb_ready(struct OGLES2Context *ctx);        /* complete; else GL_INVALID_FRAMEBUFFER_OPERATION */
BOOL ogles2_fbo_target(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb, BOOL surfaces,
                       struct OGLES2Target *t);
void ogles2_fbo_rendered(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb);
UBYTE ogles2_fbo_take_discarded(struct OGLES2Framebuffer *fb); /* OGLES2_DISCARD_* since its last job */
/* glReadPixels from the bound framebuffer object: RGBA8 rows, GL's order. */
BOOL ogles2_fbo_read(struct OGLES2Context *ctx, int x, int y, int w, int h, UBYTE *rgba, ULONG stride);
void ogles2_fbo_texture_changing(struct OGLES2Context *ctx, struct OGLES2Texture *t);
void ogles2_fbo_texture_deleted(struct OGLES2Context *ctx, struct OGLES2Texture *t);
BOOL ogles2_fbo_query(struct OGLES2Context *ctx, GLenum pname, GLint *v);
GLenum ogles2_fbo_draw_buffer(struct OGLES2Context *ctx, int i); /* GL_DRAW_BUFFERi_EXT */
void ogles2_fbo_free(struct OGLES2Context *ctx);
BOOL ogles2_texture_query(struct OGLES2Context *ctx, GLenum pname, GLint *v);
/* The texture and sampler state of texture unit `unit` (its binding of
 * OGLES2_TEX_* kind), written into the open job's state buffer; FALSE: no memory. */
BOOL ogles2_texture_state(struct OGLES2Context *ctx, int unit, int kind, ULONG *tex_state,
                          ULONG *sampler_state);
ULONG ogles2_format(char *buf, ULONG size, const char *fmt, ...); /* log.c: RawDoFmt, bounded */
/* compiler.c: the on-target shader compiler (plan 8.4) */
BOOL ogles2_cache_assigned(void);   /* OGLES2Cache: exists */
const char *ogles2_cache_dir(void); /* where compiled programs go: OGLES2Cache: or T: */
BOOL ogles2_compiler_installed(void);
ULONG ogles2_compiler_id(void); /* the installed compiler's size and date, hashed; 0: none */
BOOL ogles2_compile_program(const char *vs, const char *fs, const char *path, char *log,
                            ULONG log_size);
/* The same program's variants for samplers returning 32 bits (its .v3dw). */
BOOL ogles2_compile_program_bound(const char *vs, const char *fs, const char *bindings, const char *path,
                                  char *log, ULONG log_size);
BOOL ogles2_compile_program_wide(const char *vs, const char *fs, const char *path, char *log,
                                 ULONG log_size);
BOOL ogles2_compile_program_a2c(const char *vs, const char *fs, const char *path, char *log,
                                ULONG log_size);
/* One shader through the front end: 1 compiled, 0 GLSL errors (in log),
 * -1 the compiler could not be run. */
int ogles2_compile_shader(BOOL fragment, const char *src, char *log, ULONG log_size);
void ogles2_compiler_stop(void);
/* buffer.c */
void ogles2_buffers_free(struct OGLES2Context *ctx);
BOOL ogles2_buffer_query(struct OGLES2Context *ctx, GLenum pname, GLint *v);
struct OGLES2Buffer *ogles2_buffer_array(struct OGLES2Context *ctx);
GLuint ogles2_buffer_name(const struct OGLES2Buffer *b);
ULONG ogles2_buffer_size(const struct OGLES2Buffer *b);
const void *ogles2_buffer_indices(struct OGLES2Context *ctx, ULONG offset, ULONG bytes, BOOL *bad);
const UBYTE *ogles2_buffer_stream(const struct OGLES2Buffer *b);
ULONG ogles2_buffer_copy(struct OGLES2Context *ctx, struct OGLES2Buffer *b, ULONG offset, GLint size,
                         GLenum type, GLboolean normalized, GLsizei stride, ULONG elem_bytes,
                         ULONG *dst_stride, ULONG *vertices,
                         void (*convert)(UBYTE *dst, ULONG dst_stride, const UBYTE *src,
                                         ULONG src_stride, ULONG count, GLint size, GLenum type));
/* glClear's quad path (draw.c): for clears the tile buffer cannot do. */
void ogles2_draw_clear(struct OGLES2Context *ctx, GLbitfield mask);
/* TRUE once the open job has a draw (its tile-buffer clear would come too early). */
BOOL ogles2_job_has_draws(struct OGLES2Context *ctx);

/* ---- jobs (job.c) ---- */
void ogles2_job_begin(struct OGLES2Context *ctx);
void ogles2_job_clear(struct OGLES2Context *ctx, GLbitfield mask);
BOOL ogles2_job_flush(struct OGLES2Context *ctx, BOOL frame_end); /* FALSE: dropped. Mid-frame (frame_end FALSE): depth and stencil are kept for the next job */
void ogles2_job_free(struct OGLES2Context *ctx); /* aglDestroyContext: job.c's memory */
void ogles2_job_discard(struct OGLES2Context *ctx);
/* The submitted render waited for (M6 overlap): before the CPU reads what it
 * writes or changes what it reads. */
void ogles2_job_finish(struct OGLES2Context *ctx);
/* A job not rendered yet (open, or submitted and running) of any context in
 * ctx's share group is job `serial`: that context, else NULL. Serials are
 * unique across contexts. */
struct OGLES2Context *ogles2_job_user(struct OGLES2Context *ctx, ULONG serial);
#define ogles2_job_uses(ctx, serial) (ogles2_job_user((ctx), (serial)) != 0)

/* ---- share groups (share.c) ---- */
/* ctx joins `with`'s group, or a new one of its own (with NULL). FALSE: no memory. */
BOOL ogles2_share_join(struct OGLES2Context *ctx, struct OGLES2Context *with);
/* ctx leaves its group; TRUE if it was the last member (the shared objects go). */
BOOL ogles2_share_leave(struct OGLES2Context *ctx);
/* The semaphore a call into ctx takes (the dispatch table's wrappers): its
 * group's while it has other members, else none. An invalid ctx: none (the
 * entry itself refuses it). */
static inline struct SignalSemaphore *ogles2_share_lock_of(struct OGLES2Context *ctx)
{
    return ctx && ctx->magic == OGLES2_CONTEXT_MAGIC ? ctx->share_lock : 0;
}
/* The deferred free of a GPU block a job not rendered yet may read: on the
 * retire list of the member whose job last used it (serial), else freed now. */
struct v3d_mem;
void ogles2_share_defer_free(struct OGLES2Context *ctx, ULONG serial, struct v3d_mem *m);
/* Name tables (share.c): the slot of an object's name in its table, the
 * name itself below OGLES2_NEAR_NAMES; a larger name's (create: a new one,
 * the table then grows to it) through the table's list. OGLES2_NO_SLOT:
 * none, or no memory. */
#define OGLES2_NEAR_NAMES 65536UL
#define OGLES2_NO_SLOT    0xFFFFFFFFUL
GLuint ogles2_name_slot(struct OGLES2FarName **far, GLuint name, BOOL create);
void ogles2_name_forget(struct OGLES2FarName **far, GLuint name);   /* the name's slot is free again */
void ogles2_names_free(struct OGLES2FarName **far);

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;
extern struct OGLES2Library *OGLES2LibBase;

/* Sets the sticky error if none is pending (GLES2 section 2.5). */
void ogles2_error(struct OGLES2Context *ctx, GLenum error);
/* A context pointer the library handed out and that is still alive. Inline:
 * every entry tests it, and a call costs more than the test under Emu68. */
static inline BOOL ogles2_context_valid(struct OGLES2Context *ctx)
{
    return ctx && ctx->magic == OGLES2_CONTEXT_MAGIC;
}

/* ---- device (device.c) ----
 * Initialises the V3D on first use (powers it on, reads its identity);
 * FALSE if there is none or it did not start. */
BOOL ogles2_device_open(void);
/* GL_RENDERER: "V3D 4.2.14 (PiStorm), 8 QPUs, 64 KB VPM" */
const char *ogles2_device_renderer(void);
/* [addr, addr+size) lies in RAM the GPU reaches (the device tree's memory node). */
BOOL ogles2_device_gpu_reachable(ULONG addr, ULONG size);
/* The public "V3D 4.2 submit" semaphore (v3dcore/include/v3d_gpu_lock.h):
 * held from a job's binning submit until its render has finished. */
struct SignalSemaphore *ogles2_device_gpu_lock(void);
/* TRUE once the device initialised (a state-only context may have none). */
BOOL ogles2_device_ready(void);
/* The Pi's 1 MHz system timer: microseconds, wrapping every 71 minutes
 * (take differences as ULONG). */
ULONG ogles2_clock_us(void);
ULONG ogles2_device_bytes(void); /* GPU memory in use */
void ogles2_trace(struct OGLES2Context *ctx, const char *what, LONG n, ULONG first); /* probe T */
void ogles2_device_reset(const char *why); /* the GPU did not finish a job: reset it (lock held) */
/* v3dcore's device (struct V3DDevice); valid after ogles2_device_open. */
struct V3DDevice *ogles2_device(void);
/* Allocates a context's GPU pools and control lists for a w x h default
 * framebuffer with depth_bits 16 (D16) or 32 (D32F); NULL if out of memory. */
struct V3DContext *ogles2_device_context_new(UWORD w, UWORD h, int depth_bits, BOOL msaa);
void ogles2_device_context_free(struct V3DContext *v);
/* Sizes the pools for a new w x h (no frame open); FALSE if out of memory,
 * the old size kept where it can be. */
BOOL ogles2_device_context_resize(struct V3DContext *v, UWORD w, UWORD h);

/* ---- log (log.c; plan section 10) ----
 * Lines go to a RAM ring buffer, appended to the log file (T:ogles2.log, or
 * the path in ENV:OGLES2/LogFile) when a context is destroyed, when the
 * library is closed and when the OGLES2Log command asks. Builds with
 * OGLES2_LOG_SYNC write every line at once, so a hang loses nothing.
 * Formats are exec RawDoFmt's: %ld/%lu/%lx for 32-bit values, %s, %c. */
BOOL ogles2_log_init(void);
void ogles2_log_term(void);
void ogles2_log(const char *fmt, ...);
/* Appends the buffered lines to the log file. Needs a DOS process outside
 * Forbid(); returns FALSE (and keeps the lines) when called from a plain task.
 * Never call it from the library's Open/Close/Expunge vectors. */
BOOL ogles2_log_flush(void);

#endif
