/*
 * .v3dp writer; the format is in FORMAT.md, whose section and field order
 * this file follows. Every value is written through put_*(), which emit
 * little-endian bytes from host values, never memory images of structs.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main/mtypes.h"
#include "main/shader_types.h"
#include "program/prog_parameter.h"
#include "compiler/glsl_types.h"
#include "compiler/v3d_compiler.h"
#include "util/mesa-blake3.h"
#include "v3dp_writer.h"

#ifndef V3DP_MESA_COMMIT
#define V3DP_MESA_COMMIT "unknown"
#endif

#define V3DP_MAJOR 1
#define V3DP_MINOR 0
#define V3DP_V3D_VER 42

enum {
    SEC_PROGRAM = 1,
    SEC_STRINGS = 2,
    SEC_STAGE_PARAMS = 3,
    SEC_UNIFORMS = 4,
    SEC_ATTRIBUTES = 5,
    SEC_VARIANT = 6,
};

enum { VARIANT_VS = 0, VARIANT_VS_BIN = 1, VARIANT_FS = 4 };

/* The file numbers stages as Mesa does; the fixed-size arrays below are part
 * of the format, so a Mesa that changes them needs a new format version. */
static_assert(MESA_SHADER_VERTEX == 0 && MESA_SHADER_FRAGMENT == 4, "stage numbers");
static_assert(V3D_MAX_ANY_STAGE_INPUTS == 64, "vs key used_outputs");
static_assert(V3D_MAX_VS_INPUTS == 64 && V3D_MAX_FS_INPUTS == 64, "inputs");
static_assert(V3D_MAX_DRAW_BUFFERS == 8, "draw buffers");
static_assert(STATE_LENGTH == 4, "state indexes");

/* Mesa's hook (patch 0009). */
extern void (*v3d_compiled_variant_hook)(mesa_shader_stage stage,
                                         const struct v3d_key *key, size_t key_size,
                                         const struct v3d_prog_data *prog_data,
                                         const uint64_t *qpu_insts, uint32_t qpu_size);

/* ---- byte buffer ---- */

struct buf {
    uint8_t *p;
    size_t len, cap;
    int failed;
};

static void put_bytes(struct buf *b, const void *data, size_t n)
{
    if (b->failed)
        return;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        uint8_t *p;
        while (cap < b->len + n)
            cap *= 2;
        p = realloc(b->p, cap);
        if (!p) {
            b->failed = 1;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

static void put_u8(struct buf *b, unsigned v)
{
    uint8_t x = (uint8_t)v;
    put_bytes(b, &x, 1);
}

static void put_u16(struct buf *b, unsigned v)
{
    uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put_bytes(b, x, 2);
}

static void put_u32(struct buf *b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put_bytes(b, x, 4);
}

static void put_u64(struct buf *b, uint64_t v)
{
    put_u32(b, (uint32_t)v);
    put_u32(b, (uint32_t)(v >> 32));
}

static void put_pad4(struct buf *b)
{
    while (b->len & 3)
        put_u8(b, 0);
}

static void buf_free(struct buf *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

/* CRC-32 as zlib's crc32() (reflected, polynomial 0xedb88320). */
static uint32_t crc32_le(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) {
        int k;
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return c ^ 0xffffffffu;
}

/* ---- captured variants ---- */

#define MAX_VARIANTS 64

static struct buf variants[MAX_VARIANTS];
static unsigned num_variants;
static int capture_failed;

static void put_key_common(struct buf *b, const struct v3d_key *k)
{
    put_u32(b, k->sampler_is_32b);
    put_u8(b, k->is_last_geometry_stage);
    put_u8(b, k->robust_uniform_access);
    put_u8(b, k->robust_storage_access);
    put_u8(b, k->robust_image_access);
    put_u8(b, k->robust_image_access_2);
    put_u8(b, k->null_descriptor);
    put_u16(b, 0);
}

static void put_prog_data_common(struct buf *b, const struct v3d_prog_data *d)
{
    put_u32(b, d->spill_size);
    put_u32(b, d->tmu_spills);
    put_u32(b, d->tmu_fills);
    put_u32(b, d->tmu_count);
    put_u32(b, d->qpu_read_stalls);
    put_u8(b, d->compile_strategy_idx);
    put_u8(b, d->threads);
    put_u8(b, d->single_seg);
    put_u8(b, d->tmu_dirty_rcl);
    put_u8(b, d->has_control_barrier);
    put_u8(b, d->has_global_address);
    put_u16(b, 0);
}

static void put_vs(struct buf *b, const struct v3d_vs_key *k, const struct v3d_vs_prog_data *d)
{
    unsigned i;

    put_key_common(b, &k->base);
    put_u8(b, k->num_used_outputs);
    put_u8(b, 0);
    put_u16(b, 0);
    for (i = 0; i < 64; i++)
        put_u8(b, k->used_outputs[i].slot_and_component);
    put_u32(b, k->va_swap_rb_mask);
    put_u8(b, k->is_coord);
    put_u8(b, k->per_vertex_point_size);
    put_u8(b, k->clamp_color);
    put_u8(b, 0);

    put_prog_data_common(b, &d->base);
    put_u8(b, d->uses_iid);
    put_u8(b, d->uses_biid);
    put_u8(b, d->uses_vid);
    put_u8(b, d->separate_segments);
    put_u8(b, d->vcm_cache_size);
    put_u8(b, d->writes_psiz);
    put_u16(b, 0);
    for (i = 0; i < 16; i++)
        put_u8(b, d->vattr_sizes[i]);
    put_u32(b, d->vpm_input_size);
    put_u32(b, d->vpm_output_size);
    for (i = 0; i < 64; i++)
        put_u32(b, (uint32_t)d->driver_location_map[i]);
}

static void put_fs(struct buf *b, const struct v3d_fs_key *k, const struct v3d_fs_prog_data *d)
{
    unsigned i;

    put_key_common(b, &k->base);
    put_u8(b, k->is_points);
    put_u8(b, k->is_lines);
    put_u8(b, k->line_smoothing);
    put_u8(b, k->point_coord_upper_left);
    put_u8(b, k->msaa);
    put_u8(b, k->sample_alpha_to_coverage);
    put_u8(b, k->sample_alpha_to_one);
    put_u8(b, k->can_earlyz_with_discard);
    put_u8(b, k->software_blend);
    put_u8(b, k->ignore_sample_mask);
    put_u8(b, k->cbufs);
    put_u8(b, k->swap_color_rb);
    put_u8(b, k->f32_color_rb);
    put_u8(b, k->norm_16);
    put_u8(b, k->snorm);
    put_u8(b, k->ucp_enables);
    put_u8(b, k->has_gs);
    put_u8(b, 0);
    put_u16(b, 0);
    for (i = 0; i < 8; i++) {
        put_u32(b, k->color_fmt[i].format);
        put_bytes(b, k->color_fmt[i].swizzle, 4);
    }
    for (i = 0; i < 8; i++) {
        put_u32(b, k->blend[i].rgb_func);
        put_u32(b, k->blend[i].rgb_src_factor);
        put_u32(b, k->blend[i].rgb_dst_factor);
        put_u32(b, k->blend[i].alpha_func);
        put_u32(b, k->blend[i].alpha_src_factor);
        put_u32(b, k->blend[i].alpha_dst_factor);
    }
    put_u32(b, k->logicop_func);
    put_u32(b, k->point_sprite_mask);

    put_prog_data_common(b, &d->base);
    put_u8(b, d->uses_pid);
    put_u8(b, d->num_inputs);
    put_u8(b, d->writes_z);
    put_u8(b, d->writes_z_from_fep);
    put_u8(b, d->disable_ez);
    put_u8(b, d->uses_center_w);
    put_u8(b, d->uses_implicit_point_line_varyings);
    put_u8(b, d->lock_scoreboard_on_first_thrsw);
    put_u8(b, d->force_per_sample_msaa);
    put_u8(b, 0);
    put_u16(b, 0);
    for (i = 0; i < 64; i++)
        put_u8(b, d->input_slots[i].slot_and_component);
    for (i = 0; i < 3; i++)
        put_u32(b, d->flat_shade_flags[i]);
    for (i = 0; i < 3; i++)
        put_u32(b, d->noperspective_flags[i]);
    for (i = 0; i < 3; i++)
        put_u32(b, d->centroid_flags[i]);
}

static void variant_hook(mesa_shader_stage stage, const struct v3d_key *key, size_t key_size,
                         const struct v3d_prog_data *prog_data,
                         const uint64_t *qpu_insts, uint32_t qpu_size)
{
    struct buf *b;
    unsigned i;

    if (num_variants == MAX_VARIANTS) {
        fprintf(stderr, "v3dp: more than %d variants\n", MAX_VARIANTS);
        capture_failed = 1;
        return;
    }
    b = &variants[num_variants++];
    memset(b, 0, sizeof(*b));

    if (stage == MESA_SHADER_VERTEX && key_size == sizeof(struct v3d_vs_key)) {
        const struct v3d_vs_key *k = (const struct v3d_vs_key *)key;
        put_u32(b, k->is_coord ? VARIANT_VS_BIN : VARIANT_VS);
        put_vs(b, k, (const struct v3d_vs_prog_data *)prog_data);
    } else if (stage == MESA_SHADER_FRAGMENT && key_size == sizeof(struct v3d_fs_key)) {
        put_u32(b, VARIANT_FS);
        put_fs(b, (const struct v3d_fs_key *)key, (const struct v3d_fs_prog_data *)prog_data);
    } else {
        fprintf(stderr, "v3dp: unexpected variant (stage %d, key size %u)\n", (int)stage,
                (unsigned)key_size);
        capture_failed = 1;
        return;
    }

    put_u32(b, prog_data->uniforms.count);
    for (i = 0; i < prog_data->uniforms.count; i++)
        put_u32(b, prog_data->uniforms.contents[i]);
    for (i = 0; i < prog_data->uniforms.count; i++)
        put_u32(b, prog_data->uniforms.data[i]);

    put_u32(b, qpu_size);
    for (i = 0; i < qpu_size / 8; i++)
        put_u64(b, qpu_insts[i]);

    if (b->failed) {
        capture_failed = 1;
        return;
    }
    /* While the hook is set the driver recompiles on every request (patch
     * 0009), so draws that compute the same key report the same variant
     * again. Keep the first; the compile is deterministic. */
    for (i = 0; i + 1 < num_variants; i++) {
        if (variants[i].len == b->len && !memcmp(variants[i].p, b->p, b->len)) {
            buf_free(b);
            num_variants--;
            return;
        }
    }
}

void v3dp_capture_begin(void)
{
    unsigned i;
    for (i = 0; i < num_variants; i++)
        buf_free(&variants[i]);
    num_variants = 0;
    capture_failed = 0;
    v3d_compiled_variant_hook = variant_hook;
}

void v3dp_capture_end(void)
{
    v3d_compiled_variant_hook = NULL;
}

/* ---- program tables ---- */

/* Offset of s in the string table; the table starts with the empty string. */
static uint32_t str_add(struct buf *strings, const char *s)
{
    uint32_t off;
    if (!s || !*s)
        return 0;
    off = (uint32_t)strings->len;
    put_bytes(strings, s, strlen(s) + 1);
    return off;
}

static int stage_index(mesa_shader_stage stage)
{
    return stage == MESA_SHADER_VERTEX ? 0 : stage == MESA_SHADER_FRAGMENT ? 1 : -1;
}

static const mesa_shader_stage stages_in_file[2] = { MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT };

static int put_stage_params(struct buf *b, struct buf *strings, mesa_shader_stage stage,
                            const struct gl_program_parameter_list *pl)
{
    unsigned i, j;
    uint32_t *values = calloc(pl->NumParameterValues + 1, sizeof(uint32_t));

    if (!values)
        return -1;
    for (i = 0; i < pl->NumParameterValues; i++)
        values[i] = pl->ParameterValues[i].u;

    put_u32(b, stage);
    put_u32(b, pl->NumParameterValues);
    put_u32(b, pl->NumParameters);
    for (i = 0; i < pl->NumParameters; i++) {
        const struct gl_program_parameter *p = &pl->Parameters[i];
        unsigned kind;
        switch (p->Type) {
        case PROGRAM_UNIFORM:   kind = 0; break;
        case PROGRAM_CONSTANT:  kind = 1; break;
        case PROGRAM_STATE_VAR: kind = 2; break;
        default:
            fprintf(stderr, "v3dp: parameter %s has register file %d\n",
                    p->Name ? p->Name : "?", (int)p->Type);
            free(values);
            return -1;
        }
        /* The runtime computes state variables before each draw; the
         * template holds 0 there whatever the link left behind. */
        if (kind == 2) {
            unsigned n = p->Padded ? (p->Size + 3u) & ~3u : p->Size;
            for (j = 0; j < n && p->ValueOffset + j < pl->NumParameterValues; j++)
                values[p->ValueOffset + j] = 0;
        }
        put_u32(b, kind);
        put_u32(b, str_add(strings, p->Name));
        put_u32(b, p->ValueOffset);
        put_u32(b, p->Size);
        put_u32(b, p->DataType);
        for (j = 0; j < STATE_LENGTH; j++)
            put_u16(b, kind == 2 ? (uint16_t)p->StateIndexes[j] : 0);
    }
    for (i = 0; i < pl->NumParameterValues; i++)
        put_u32(b, values[i]);
    free(values);
    return 0;
}

static int put_uniforms(struct buf *b, struct buf *strings, struct gl_shader_program *prog)
{
    const struct gl_shader_program_data *data = prog->data;
    unsigned i, j, s;

    put_u32(b, data->NumUniformStorage);
    for (i = 0; i < data->NumUniformStorage; i++) {
        const struct gl_uniform_storage *u = &data->UniformStorage[i];
        const struct glsl_type *base = glsl_without_array(u->type);
        int sampler = glsl_type_is_sampler(base);

        put_u32(b, str_add(strings, u->name.string));
        put_u32(b, u->type->gl_type);
        put_u32(b, u->array_elements);
        put_u32(b, (sampler ? 1u : 0u) | (u->builtin ? 2u : 0u) | (u->hidden ? 4u : 0u));
        put_u32(b, u->num_driver_storage);
        for (j = 0; j < u->num_driver_storage; j++) {
            const struct gl_uniform_driver_storage *ds = &u->driver_storage[j];
            int found = -1;
            uint32_t offset = 0;
            for (s = 0; s < 2 && found < 0; s++) {
                const struct gl_linked_shader *sh = prog->_LinkedShaders[stages_in_file[s]];
                const struct gl_program_parameter_list *pl;
                const union gl_constant_value *pv;
                if (!sh)
                    continue;
                pl = sh->Program->Parameters;
                pv = (const union gl_constant_value *)ds->data;
                if (pv >= pl->ParameterValues &&
                    pv < pl->ParameterValues + pl->NumParameterValues) {
                    found = (int)s;
                    offset = (uint32_t)(pv - pl->ParameterValues);
                }
            }
            if (found < 0) {
                fprintf(stderr, "v3dp: uniform %s: storage outside the stages' constants\n",
                        u->name.string);
                return -1;
            }
            put_u32(b, stages_in_file[found]);
            put_u32(b, offset);
            put_u8(b, ds->element_stride);
            put_u8(b, ds->vector_stride);
            put_u8(b, ds->format);
            put_u8(b, 0);
        }
        for (s = 0; s < 2; s++) {
            mesa_shader_stage st = stages_in_file[s];
            put_u32(b, sampler && u->opaque[st].active ? u->opaque[st].index : 0xffffffffu);
        }
    }
    return 0;
}

static int put_attributes(struct buf *b, struct buf *strings, struct gl_shader_program *prog)
{
    const struct gl_linked_shader *vs = prog->_LinkedShaders[MESA_SHADER_VERTEX];
    const struct gl_program_resource *res = prog->data->ProgramResourceList;
    uint64_t inputs_read = vs ? vs->Program->info.inputs_read : 0;
    unsigned i, n = 0;

    for (i = 0; i < prog->data->NumProgramResourceList; i++)
        if (res[i].Type == GL_PROGRAM_INPUT &&
            ((const struct gl_shader_variable *)res[i].Data)->location >= 0)
            n++;

    put_u32(b, n);
    for (i = 0; i < prog->data->NumProgramResourceList; i++) {
        const struct gl_shader_variable *var = res[i].Data;
        unsigned attr;
        if (res[i].Type != GL_PROGRAM_INPUT || var->location < 0)
            continue;
        /* GL resource locations of vertex inputs are generic attribute
         * numbers; the driver's input slot is the number of inputs read
         * below it (nir_recompute_io_bases, st_atom_array.cpp). */
        attr = VERT_ATTRIB_GENERIC0 + (unsigned)var->location;
        /* A matrix's columns are inputs of their own, and one the shader
         * does not read has no slot (mat2(m3) reads two of three): the
         * location word carries which columns are read (FORMAT.md), the
         * slot is that of the first of them. */
        {
            unsigned cols = var->type->matrix_columns > 1 ? var->type->matrix_columns : 1, k, mask = 0;
            int first = -1;
            for (k = 0; k < cols; k++)
                if ((inputs_read >> (attr + k)) & 1) {
                    mask |= 1u << k;
                    if (first < 0)
                        first = (int)k;
                }
            put_u32(b, str_add(strings, var->name.string));
            put_u32(b, var->type->gl_type);
            put_u32(b, (uint32_t)var->location | (mask << 8) | 0x80000000u);
            put_u32(b, first >= 0 ? (uint32_t)util_bitcount64(inputs_read & BITFIELD64_MASK(attr + first))
                                  : 0xffffffffu);
        }
    }
    return 0;
}

static void put_section(struct buf *out, uint32_t type, const struct buf *payload)
{
    put_u32(out, type);
    put_u32(out, (uint32_t)payload->len);
    put_bytes(out, payload->p, payload->len);
    put_pad4(out);
}

int v3dp_write(const char *path, struct gl_shader_program *prog, unsigned num_sources,
               const unsigned *stages, const char *const *sources)
{
    /* program = PROGRAM section, body = the sections after STRINGS (STRINGS
     * is complete only once they are written). */
    struct buf program = { 0 }, body = { 0 }, strings = { 0 }, sec = { 0 }, file = { 0 };
    struct mesa_blake3 h;
    blake3_hash hash;
    char commit[12];
    unsigned i, num_sections = 0, stage_mask = 0;
    int rc = -1;
    FILE *f;

    if (capture_failed) {
        fprintf(stderr, "v3dp: variant capture failed\n");
        return -1;
    }
    put_u8(&strings, 0);

    /* PROGRAM */
    _mesa_blake3_init(&h);
    for (i = 0; i < num_sources; i++) {
        struct buf hb = { 0 };
        put_u32(&hb, stages[i]);
        put_u32(&hb, (uint32_t)strlen(sources[i]));
        if (hb.failed)
            goto out;
        _mesa_blake3_update(&h, hb.p, hb.len);
        _mesa_blake3_update(&h, sources[i], strlen(sources[i]));
        buf_free(&hb);
    }
    _mesa_blake3_final(&h, hash);
    for (i = 0; i < 2; i++)
        if (prog->_LinkedShaders[stages_in_file[i]])
            stage_mask |= 1u << stages_in_file[i];
    put_bytes(&sec, hash, sizeof(hash));
    put_u32(&sec, stage_mask);
    put_section(&program, SEC_PROGRAM, &sec);
    num_sections++;
    buf_free(&sec);

    /* STAGE_PARAMS */
    for (i = 0; i < 2; i++) {
        const struct gl_linked_shader *sh = prog->_LinkedShaders[stages_in_file[i]];
        if (!sh)
            continue;
        if (put_stage_params(&sec, &strings, stages_in_file[i], sh->Program->Parameters))
            goto out;
        put_section(&body, SEC_STAGE_PARAMS, &sec);
        num_sections++;
        buf_free(&sec);
    }

    /* UNIFORMS, ATTRIBUTES */
    if (put_uniforms(&sec, &strings, prog))
        goto out;
    put_section(&body, SEC_UNIFORMS, &sec);
    num_sections++;
    buf_free(&sec);
    if (put_attributes(&sec, &strings, prog))
        goto out;
    put_section(&body, SEC_ATTRIBUTES, &sec);
    num_sections++;
    buf_free(&sec);

    /* VARIANT, in compile order */
    for (i = 0; i < num_variants; i++) {
        put_section(&body, SEC_VARIANT, &variants[i]);
        num_sections++;
    }

    /* PROGRAM, STRINGS, the rest: everything the header's CRC covers */
    put_section(&program, SEC_STRINGS, &strings);
    num_sections++;
    put_bytes(&program, body.p, body.len);
    if (program.failed || body.failed || strings.failed) {
        fprintf(stderr, "v3dp: out of memory\n");
        goto out;
    }

    /* Header */
    memset(commit, 0, sizeof(commit));
    memcpy(commit, V3DP_MESA_COMMIT,
           strlen(V3DP_MESA_COMMIT) < sizeof(commit) ? strlen(V3DP_MESA_COMMIT) : sizeof(commit));
    put_bytes(&file, "V3DP", 4);
    put_u16(&file, V3DP_MAJOR);
    put_u16(&file, V3DP_MINOR);
    put_u32(&file, V3DP_V3D_VER);
    put_bytes(&file, commit, sizeof(commit));
    put_u32(&file, num_sections);
    put_u32(&file, crc32_le(program.p, program.len));
    put_bytes(&file, program.p, program.len);
    if (file.failed) {
        fprintf(stderr, "v3dp: out of memory\n");
        goto out;
    }

    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "v3dp: cannot create %s\n", path);
        goto out;
    }
    if (fwrite(file.p, 1, file.len, f) != file.len) {
        fprintf(stderr, "v3dp: write error on %s\n", path);
        fclose(f);
        goto out;
    }
    if (fclose(f)) {
        fprintf(stderr, "v3dp: write error on %s\n", path);
        goto out;
    }
    fprintf(stderr, "v3dp: wrote %s (%u bytes, %u variants)\n", path, (unsigned)file.len,
            num_variants);
    rc = 0;
out:
    buf_free(&program);
    buf_free(&body);
    buf_free(&strings);
    buf_free(&sec);
    buf_free(&file);
    return rc;
}
