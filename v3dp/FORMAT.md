# `.v3dp` program binary container, version 1.0

**Status: frozen (2026-10-10).** Any change follows the versioning rules at the end. Verified:
the x86-64 and m68k writers produce identical bytes (`dev/spike-c/harness/compare_v3dp.sh`, five
test programs; the A1200 matched for the first four), and `v3dp_dump.py`, written from this
document, decodes every file.

A `.v3dp` file holds one linked GLES2 program compiled for V3D 4.2: every compiled shader
variant (QPU code, uniform recipe, the shader properties the draw path needs) and the program
tables the runtime needs to implement `glUniform*`, `glGetUniformLocation`, samplers and
attributes. It is written by the offline compiler (S1, `v3dglslc` on the PC) and by the
on-target compiler module (S2), and read by `ogles2.library` (plan sections 8.2 to 8.4).

**Rule zero: both compilers must produce the same bytes.** The writer serializes every field
explicitly; nothing is a memory image of a host struct (the hosts differ in byte order and in
struct alignment).

## Encoding

- All integers are **little-endian**. `u8`/`u16`/`u32`/`i16`/`i32`/`u64`.
- QPU instructions are `u64`, little-endian, exactly as the GPU reads them.
- Strings live in the string table (section `STRINGS`) and are referenced by `str`, a `u32`
  byte offset into it. Offset 0 is the empty string.
- Every section starts 4-byte aligned; padding bytes are 0.
- Floats are stored as their IEEE-754 bit patterns in `u32`.

## File layout

```
header
section 0
section 1
...
```

### Header (32 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | `char[4]` | magic `"V3DP"` |
| 4 | `u16` | format major version (1) |
| 6 | `u16` | format minor version (0) |
| 8 | `u32` | V3D version × 10 + revision, as Mesa's `devinfo.ver` (42) |
| 12 | `char[12]` | Mesa commit, first 12 hex digits, NUL-padded |
| 24 | `u32` | number of sections |
| 28 | `u32` | CRC-32 (zlib's `crc32()`: reflected, polynomial 0xedb88320) of all bytes after the header |

A reader rejects a major version it does not know, a V3D version other than 42, and a Mesa
commit not on its list of validated commits (the runtime may then recompile with S2, or
report the miss for S1).

### Section

| Type | Field |
|---|---|
| `u32` | section type |
| `u32` | payload size in bytes (excluding padding) |
| bytes | payload, then zero padding to a multiple of 4 |

Unknown section types are skipped (forward compatibility within a major version).

| Type | Name | Count |
|---|---|---|
| 1 | `PROGRAM` | exactly 1, first |
| 2 | `STRINGS` | exactly 1, second |
| 3 | `STAGE_PARAMS` | one per linked stage (vertex, fragment) |
| 4 | `UNIFORMS` | exactly 1 |
| 5 | `ATTRIBUTES` | exactly 1 |
| 6 | `VARIANT` | one per compiled variant |

## `PROGRAM`

| Type | Field |
|---|---|
| `u8[32]` | BLAKE3 of the program's sources: for each shader in attach order, its stage (`u32`), its length in bytes (`u32`) and its text. The runtime's disk-cache file name is the hash's first 24 hex digits plus `.v3dp` (29 characters: FFS file names are at most 30); the runtime checks the full hash in the file |
| `u32` | stages present: bit 0 vertex, bit 4 fragment (Mesa's `mesa_shader_stage` bit numbers) |

## `STRINGS`

Concatenated NUL-terminated strings. The first byte is NUL (offset 0 = empty string).

## `STAGE_PARAMS`: one stage's constant buffer

The driver reads uniforms (`QUNIFORM_UNIFORM`) from a per-stage constant buffer of 32-bit
values: Mesa's `gl_program_parameter_list::ParameterValues`. The runtime keeps one per stage,
initialised from the template below, updates it from `glUniform*` (table `UNIFORMS`) and from
GL state before each draw (state entries below).

| Type | Field |
|---|---|
| `u32` | stage (0 vertex, 4 fragment) |
| `u32` | number of 32-bit values in the constant buffer (`NumParameterValues`) |
| `u32` | number of parameters (`NumParameters`) |
| per parameter: | |
| `u32` | kind: 0 uniform, 1 constant, 2 state variable (Mesa `PROGRAM_UNIFORM`, `PROGRAM_CONSTANT`, `PROGRAM_STATE_VAR`) |
| `str` | name |
| `u32` | value offset in the constant buffer, in 32-bit units (`ValueOffset`) |
| `u32` | size in components (`Size`) |
| `u32` | GL data type (`DataType`, e.g. `GL_FLOAT_VEC4`) |
| `i16[4]` | state indexes (`StateIndexes`, Mesa `gl_state_index` values); zeros unless kind 2 |
| `u32[n]` | template: the constant buffer's initial contents. Constants are filled in, uniforms hold the linker's initial values (0 in GLES2), state variables hold 0 |

The runtime must compute every state variable it is given before a draw, and must reject a
program containing a state index it does not implement (plan section 7.11), rather than leave
it at zero. State index numbers are Mesa's enum values at the validated commit, like the
recipe types below.

## `UNIFORMS`: the program's active uniforms

| Type | Field |
|---|---|
| `u32` | number of uniforms (Mesa's `UniformStorage` entries, in its order) |
| per uniform: | |
| `str` | name as Mesa stores it (an array of a basic type without `[0]`, struct members as `s.m`, `s[1].m`) |
| `u32` | GL type (`GL_FLOAT_MAT4`, `GL_SAMPLER_2D`, ...) of one element |
| `u32` | array elements (0 if not an array) |
| `u32` | flags: bit 0 sampler, bit 1 built-in (`gl_*`; a state variable, no storage places), bit 2 hidden (not visible to the application) |
| `u32` | number of storage places |
| per storage place: | where `glUniform*` writes the value; one per stage that uses the uniform |
| `u32` | stage |
| `u32` | offset in that stage's constant buffer, 32-bit units |
| `u8` | element stride in **bytes** (between array elements) |
| `u8` | vector stride in **bytes** (between matrix columns) |
| `u8` | format: 0 native, 1 integer stored as float (Mesa `gl_uniform_driver_format`) |
| `u8` | pad |
| `u32[2]` | sampler index for the vertex and the fragment stage (the texture index in that stage's `TMU_CONFIG` recipe data); 0xffffffff where not a sampler or unused there |

Samplers have no storage places: the texture unit a sampler reads (`glUniform1i`) is runtime
state, not part of a constant buffer. Built-in uniforms (`gl_DepthRange.near`, ...) have none
either; the shader reads them through state variables in `STAGE_PARAMS`.

Uniform locations are the runtime's own numbering; the file does not fix them. The strides
are the layout; with the V3D driver at the validated commit every vector, array element and
matrix column starts on 4 values (16 bytes), e.g. a `mat3` takes 12 values and a `vec3[4]` 16.

## `ATTRIBUTES`: vertex shader inputs

| Type | Field |
|---|---|
| `u32` | number of attributes |
| per attribute: | |
| `str` | name |
| `u32` | GL type |
| `u32` | GL location the linker assigned (informational) |
| `u32` | input slot of its first column: the number of vertex inputs the shader reads below this one (`inputs_read` bit order, as `nir_recompute_io_bases` numbers them); 0xffffffff if the shader does not read it |

The runtime maps an application's attribute arrays to input slots **by name**, so a binary
does not depend on `glBindAttribLocation`. A matrix attribute takes one slot per column.

## `VARIANT`: one compiled shader

| Type | Field |
|---|---|
| `u32` | stage: 0 vertex, 1 vertex binning (coordinate) shader, 4 fragment |
| key | the variant key, field by field (below) |
| prog data | the shader's properties, field by field (below) |
| `u32` | uniform recipe entries |
| `u32[n]`, `u32[n]` | recipe types (`enum quniform_contents` values), then their data words |
| `u32` | QPU code size in bytes |
| `u64[size/8]` | QPU instructions, little-endian |

Recipe type numbers are Mesa's enum values at the validated commit (0 `QUNIFORM_CONSTANT`
to 45 `QUNIFORM_BLEND_CONSTANT_A` at 74b69717c52b; `v3dp_dump.py` lists them); the reader
checks the commit (header) and rejects types it does not implement (plan section 8.6). For
`QUNIFORM_UNIFORM` the data word is an index, in 32-bit units, into the stage's constant
buffer (`STAGE_PARAMS`); for `QUNIFORM_CONSTANT` it is the value itself.

Variants appear in the order the driver compiled them, without duplicates. The runtime uses
a variant only when its key equals the key the draw computes, field by field; anything else
is a miss (compile with S2, or report it).

What a file contains is the writer's choice, not part of the format. `v3dglslc` draws once
per default key state and stores what the driver compiles for those draws: triangles, lines
and points into an RGBA8 framebuffer object with a depth buffer, each sampler on its own unit
with a complete RGBA8 texture. That gives one vertex and one binning shader (two more if the
vertex shader writes `gl_PointSize`, for points) and a fragment shader per primitive class.
Variants the driver compiles at link time (`V3D_DEBUG=precompile`) are not stored: their keys
never match a draw (all eight colour buffers for `gl_FragColor`, every vertex output kept).

### Common key and prog data (all stages)

Key (`struct v3d_key`): `u32 sampler_is_32b`, then `u8` each: `is_last_geometry_stage`,
`robust_uniform_access`, `robust_storage_access`, `robust_image_access`,
`robust_image_access_2`, `null_descriptor`, `u8 pad[2]`.

Prog data (`struct v3d_prog_data`): `u32` each: `spill_size`, `tmu_spills`, `tmu_fills`,
`tmu_count`, `qpu_read_stalls`; `u8` each: `compile_strategy_idx`, `threads`, `single_seg`,
`tmu_dirty_rcl`, `has_control_barrier`, `has_global_address`, `u8 pad[2]`.

### Vertex and binning shaders

Key (`struct v3d_vs_key`), after the common key: `u8 num_used_outputs`, `u8 pad[3]`,
`u8 used_outputs[64]` (`slot_and_component`), `u32 va_swap_rb_mask`, `u8` each: `is_coord`,
`per_vertex_point_size`, `clamp_color`, `u8 pad`.

Prog data (`struct v3d_vs_prog_data`), after the common part: `u8` each: `uses_iid`,
`uses_biid`, `uses_vid`, `separate_segments`, `vcm_cache_size`, `writes_psiz`, `u8 pad[2]`,
`u8 vattr_sizes[16]`, `u32 vpm_input_size`, `u32 vpm_output_size`,
`i32 driver_location_map[64]`.

### Fragment shader

Key (`struct v3d_fs_key`), after the common key: `u8` each: `is_points`, `is_lines`,
`line_smoothing`, `point_coord_upper_left`, `msaa`, `sample_alpha_to_coverage`,
`sample_alpha_to_one`, `can_earlyz_with_discard`, `software_blend`, `ignore_sample_mask`,
`cbufs`, `swap_color_rb`, `f32_color_rb`, `norm_16`, `snorm`, `ucp_enables`, `has_gs`,
`u8 pad[3]`; per draw buffer (8): `u32 format` (`enum pipe_format`), `u8 swizzle[4]`; per draw
buffer (8): `u32` each `rgb_func`, `rgb_src_factor`, `rgb_dst_factor`, `alpha_func`,
`alpha_src_factor`, `alpha_dst_factor`; `u32 logicop_func`, `u32 point_sprite_mask`.

Prog data (`struct v3d_fs_prog_data`), after the common part: `u8` each: `uses_pid`,
`num_inputs`, `writes_z`, `writes_z_from_fep`, `disable_ez`, `uses_center_w`,
`uses_implicit_point_line_varyings`, `lock_scoreboard_on_first_thrsw`,
`force_per_sample_msaa`, `u8 pad[3]`, `u8 input_slots[64]`, `u32 flat_shade_flags[3]`,
`u32 noperspective_flags[3]`, `u32 centroid_flags[3]`.

## Versioning

Adding a section type or appending fields at the end of a section is a minor version change.
Anything else is a major version change. The reader skips unknown sections and ignores
trailing bytes it does not know.
