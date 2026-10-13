/*
 * .v3dp writer (FORMAT.md): serializes a linked GLES2 program and every V3D
 * variant the driver compiled for it, field by field in little-endian, so the
 * m68k compiler module and the PC compiler produce the same bytes.
 *
 * Needs Mesa patch 0009 (v3d_compiled_variant_hook).
 */
#ifndef V3DP_WRITER_H
#define V3DP_WRITER_H

struct gl_shader_program;

/* Start capturing variants: installs the driver hook and forgets earlier ones. */
void v3dp_capture_begin(void);
/* Stop capturing (removes the hook); the captured variants stay until the next begin. */
void v3dp_capture_end(void);

/* The "stage" of the program's attribute bindings in the hash (v3dp/FORMAT.md,
 * PROGRAM): a program linked with them (V3DGLSL_LINK_BOUND) hashes them as a
 * third source. "BIND". */
#define V3DP_HASH_BINDINGS 0x42494E44u

/* Writes the program and the captured variants to path. sources[i] is the
 * source of the shader attached i-th, stages[i] its mesa_shader_stage (or
 * V3DP_HASH_BINDINGS: hashed only).
 * Returns 0, or -1 after printing the reason to stderr. */
int v3dp_write(const char *path, struct gl_shader_program *prog,
               unsigned num_sources, const unsigned *stages,
               const char *const *sources);

#endif
