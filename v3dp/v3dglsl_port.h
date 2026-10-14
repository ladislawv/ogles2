/*
 * The on-target shader compiler's message port (plan section 8.4, stage S2).
 *
 * The compiler is v3dglslc (tools/v3dglslc) started as a resident server:
 * "v3dglslc SERVER" adds the public port V3DGLSL_PORT_NAME and serves
 * requests until it gets V3DGLSL_QUIT. ogles2.library starts it on the
 * first program it has no binary for, and stops it when the library is
 * expunged. Each request carries both sources; the compiler writes the
 * program's .v3dp file (v3dp/FORMAT.md) where the request says, and the
 * library loads it like any other.
 *
 * The two sides are built by different compilers with different C
 * libraries (ogles2: GCC 6 and clib2; the compiler: GCC 16 and libnix):
 * this header uses only Exec types, and all memory belongs to the sender,
 * except a request without a reply port (mn_ReplyPort NULL: a QUIT sent
 * where the sender cannot wait, such as a library expunge), which must be
 * AllocVec'd and which the server frees with FreeVec.
 */
#ifndef V3DGLSL_PORT_H
#define V3DGLSL_PORT_H

#include <exec/ports.h>

#define V3DGLSL_PORT_NAME "v3dglsl compiler"
#define V3DGLSL_VERSION   1

#define V3DGLSL_LINK    1 /* compile vs and fs, link, write the .v3dp to out_path */
#define V3DGLSL_QUIT    2 /* remove the port and exit (queued requests fail) */
#define V3DGLSL_COMPILE 3 /* compile one shader (vs or fs, the other NULL): status and log only */
#define V3DGLSL_LINK32  4 /* as LINK, but write the variants for samplers returning 32 bits (float
                           * and depth textures): the program's .v3dw file. A compiler older than
                           * this answers 3 (bad request). */
#define V3DGLSL_LINK_A2C 6 /* as LINK32, but the alpha-to-coverage fragment variants (Mesa's key
                            * msaa and sample_alpha_to_coverage, patch 0014): the program's .v3da
                            * file, for a 4x MSAA context with GL_SAMPLE_ALPHA_TO_COVERAGE. A
                            * compiler older than this answers 3. */
#define V3DGLSL_LINK_BOUND 5 /* as LINK, glBindAttribLocation's `bindings` applied before the link
                              * (GLES 2.0 2.10.4: aliased attributes, two names bound to one
                              * location, which Mesa's linker must know of); they are part of the
                              * program's hash (v3dp/FORMAT.md). A compiler older than this
                              * answers 3. */

struct V3DGLSLRequest
{
    struct Message msg;   /* mn_ReplyPort: the sender's */
    ULONG version;        /* V3DGLSL_VERSION */
    ULONG op;             /* V3DGLSL_* */
    const char *vs, *fs;  /* GLSL ES sources, NUL-terminated, exactly as the program gave them */
    const char *out_path; /* the .v3dp file to write */
    char *log;            /* compile and link messages, NUL-terminated (may be truncated) */
    ULONG log_size;
    LONG result;          /* reply: 0 done (written); 1 compile or link failed; 2 not written; 3 bad request */
    const char *bindings; /* V3DGLSL_LINK_BOUND only (read for no other op: an older sender's
                           * request ends before it): "<index> <name>\n" lines, sorted by name */
};

#endif
