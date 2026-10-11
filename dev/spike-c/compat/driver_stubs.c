/*
 * Stubs and small runtime pieces for running Mesa's V3D driver to compile
 * shaders (spike part 2). Each one is either debug-only, display-only, or a
 * libnix gap; none is on the shader-compile path except the atomics.
 */
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>
#include <proto/exec.h>

/* --- atomics GCC does not inline for this target -------------------------
 * Forbid() makes them atomic against other tasks; nothing here runs in an
 * interrupt. */
unsigned int __sync_val_compare_and_swap_4(volatile void *ptr, unsigned int expected,
                                           unsigned int desired)
{
    volatile unsigned int *p = ptr;
    unsigned int old;
    Forbid();
    old = *p;
    if (old == expected)
        *p = desired;
    Permit();
    return old;
}

unsigned long long __sync_val_compare_and_swap_8(volatile void *ptr, unsigned long long expected,
                                                 unsigned long long desired)
{
    volatile unsigned long long *p = ptr;
    unsigned long long old;
    Forbid();
    old = *p;
    if (old == expected)
        *p = desired;
    Permit();
    return old;
}

/* --- libnix gaps ---------------------------------------------------------- */

/* util/u_thread.c blocks signals in new threads; there are no POSIX signals
 * to block on AmigaOS. */
int pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    (void)how;
    (void)set;
    if (old)
        memset(old, 0, sizeof(*old));
    return 0;
}

/* util/xmlconfig.c matches driconf application names with regex. There are no
 * driconf files here, so nothing ever matches. */
int regcomp(regex_t *preg, const char *pattern, int cflags)
{
    (void)preg;
    (void)pattern;
    (void)cflags;
    return REG_BADPAT;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch,
            regmatch_t pmatch[], int eflags)
{
    (void)preg;
    (void)string;
    (void)nmatch;
    (void)pmatch;
    (void)eflags;
    return REG_NOMATCH;
}

void regfree(regex_t *preg)
{
    (void)preg;
}

/* src/util/streaming-load-memcpy.c is an SSE4.1 helper; plain copy here. */
void util_streaming_load_memcpy(void *restrict dst, void *restrict src, size_t len)
{
    memcpy(dst, src, len);
}

/* --- command-list decoder (src/broadcom/cle/v3d_decoder.c, needs expat) ---
 * Used only for V3D_DEBUG=cl/clif dumps. */
struct v3d_spec;
struct v3d_group;
struct v3d_device_info;
struct clif_dump;

struct v3d_spec *v3d_spec_load(const struct v3d_device_info *devinfo)
{
    (void)devinfo;
    return NULL;
}

struct v3d_group *v3d_spec_find_struct(struct v3d_spec *spec, const char *name)
{
    (void)spec;
    (void)name;
    return NULL;
}

struct v3d_group *v3d_spec_find_instruction(struct v3d_spec *spec, const uint8_t *p)
{
    (void)spec;
    (void)p;
    return NULL;
}

int v3d_group_get_length(struct v3d_group *group)
{
    (void)group;
    return 0;
}

const char *v3d_group_get_name(struct v3d_group *group)
{
    (void)group;
    return "?";
}

void v3d_print_group(struct clif_dump *clif, struct v3d_group *group, uint64_t offset,
                     const uint8_t *p)
{
    (void)clif;
    (void)group;
    (void)offset;
    (void)p;
}

/* --- KMS display buffers (src/gallium/auxiliary/renderonly) --------------- */
struct renderonly;
struct renderonly_scanout;
struct pipe_resource;
struct winsys_handle;

struct renderonly_scanout *renderonly_create_gpu_import_for_resource(struct pipe_resource *rsc,
                                                                     struct renderonly *ro,
                                                                     struct winsys_handle *out)
{
    (void)rsc;
    (void)ro;
    (void)out;
    return NULL;
}

void renderonly_scanout_destroy(struct renderonly_scanout *scanout, struct renderonly *ro)
{
    (void)scanout;
    (void)ro;
}

/* --- ASTC decoding tables (texcompress_astc_luts*.cpp need std::mutex) ----
 * GLES2 has no ASTC; Mesa asks for these only when decoding ASTC textures. */
struct astc_decoder_lut_holder_stub;

void _mesa_init_astc_decoder_luts(void *holder)
{
    (void)holder;
}

void *_mesa_get_astc_decoder_partition_table(uint32_t block_width, uint32_t block_height,
                                             unsigned *lut_width, unsigned *lut_height)
{
    (void)block_width;
    (void)block_height;
    if (lut_width)
        *lut_width = 0;
    if (lut_height)
        *lut_height = 0;
    return NULL;
}
