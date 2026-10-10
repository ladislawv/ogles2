#!/usr/bin/env python3
"""Decode a .v3dp file (FORMAT.md) and print it as text.

Written from FORMAT.md, not from the writer: every section must decode to
exactly its stated size, the CRC must match, and the counts must agree, or
this exits with status 1. Usage: v3dp_dump.py file.v3dp [--qpu]
"""
import struct, sys, zlib

STAGES = {0: "vertex", 4: "fragment"}
VARIANT_STAGES = {0: "vertex", 1: "vertex binning", 4: "fragment"}
KINDS = {0: "uniform", 1: "constant", 2: "state"}
SECTIONS = {1: "PROGRAM", 2: "STRINGS", 3: "STAGE_PARAMS", 4: "UNIFORMS", 5: "ATTRIBUTES",
            6: "VARIANT"}
# enum quniform_contents at Mesa 74b69717c52b (values printed by the compiler)
QUNIFORM = ("CONSTANT UNIFORM VIEWPORT_X_SCALE VIEWPORT_Y_SCALE VIEWPORT_Z_OFFSET "
            "VIEWPORT_Z_SCALE TMU_CONFIG_P0 TMU_CONFIG_P1 IMAGE_TMU_CONFIG_P0 TEXTURE_FIRST_LEVEL "
            "TEXTURE_WIDTH TEXTURE_HEIGHT TEXTURE_DEPTH TEXTURE_ARRAY_SIZE TEXTURE_LEVELS "
            "TEXTURE_SAMPLES UBO_ADDR TEXRECT_SCALE_X TEXRECT_SCALE_Y SSBO_OFFSET GET_SSBO_SIZE "
            "GET_UBO_SIZE IMAGE_WIDTH IMAGE_HEIGHT IMAGE_DEPTH IMAGE_ARRAY_SIZE LINE_WIDTH "
            "AA_LINE_WIDTH NUM_WORK_GROUPS WORK_GROUP_BASE WORK_GROUP_SIZE SPILL_OFFSET "
            "SPILL_SIZE_PER_THREAD SHARED_OFFSET SHARED_SIZE FB_LAYERS VIEW_INDEX INLINE_UBO_0 "
            "INLINE_UBO_1 INLINE_UBO_2 INLINE_UBO_3 DRAW_ID BLEND_CONSTANT_R BLEND_CONSTANT_G "
            "BLEND_CONSTANT_B BLEND_CONSTANT_A").split()


def recipe(contents, word):
    name = QUNIFORM[contents] if contents < len(QUNIFORM) else "type%d" % contents
    return "%s(%s)" % (name, word if name == "UNIFORM" else "%08x" % word)


class Bad(Exception):
    pass


class Reader:
    def __init__(self, data, what):
        self.d, self.o, self.what = data, 0, what

    def take(self, n):
        if self.o + n > len(self.d):
            raise Bad("%s: truncated at %d (+%d of %d)" % (self.what, self.o, n, len(self.d)))
        b = self.d[self.o:self.o + n]
        self.o += n
        return b

    def u8(self): return self.take(1)[0]
    def u16(self): return struct.unpack("<H", self.take(2))[0]
    def i16(self): return struct.unpack("<h", self.take(2))[0]
    def u32(self): return struct.unpack("<I", self.take(4))[0]
    def i32(self): return struct.unpack("<i", self.take(4))[0]
    def u64(self): return struct.unpack("<Q", self.take(8))[0]
    def u8s(self, n): return list(self.take(n))
    def u32s(self, n): return [self.u32() for _ in range(n)]

    def pad(self, n):
        if any(self.take(n)):
            raise Bad("%s: nonzero padding at %d" % (self.what, self.o - n))

    def done(self):
        if self.o != len(self.d):
            raise Bad("%s: %d bytes left over" % (self.what, len(self.d) - self.o))


def string(strings, off):
    if off >= len(strings):
        raise Bad("string offset %d outside the table (%d bytes)" % (off, len(strings)))
    end = strings.index(b"\0", off)
    return strings[off:end].decode("utf-8")


def key_common(r):
    k = {"sampler_is_32b": r.u32()}
    for f in ("is_last_geometry_stage", "robust_uniform_access", "robust_storage_access",
              "robust_image_access", "robust_image_access_2", "null_descriptor"):
        k[f] = r.u8()
    r.pad(2)
    return k


def prog_data_common(r):
    d = {}
    for f in ("spill_size", "tmu_spills", "tmu_fills", "tmu_count", "qpu_read_stalls"):
        d[f] = r.u32()
    for f in ("compile_strategy_idx", "threads", "single_seg", "tmu_dirty_rcl",
              "has_control_barrier", "has_global_address"):
        d[f] = r.u8()
    r.pad(2)
    return d


def vs_variant(r):
    k = key_common(r)
    k["num_used_outputs"] = r.u8()
    r.pad(3)
    k["used_outputs"] = r.u8s(64)[:k["num_used_outputs"]]
    k["va_swap_rb_mask"] = r.u32()
    for f in ("is_coord", "per_vertex_point_size", "clamp_color"):
        k[f] = r.u8()
    r.pad(1)
    d = prog_data_common(r)
    for f in ("uses_iid", "uses_biid", "uses_vid", "separate_segments", "vcm_cache_size",
              "writes_psiz"):
        d[f] = r.u8()
    r.pad(2)
    d["vattr_sizes"] = r.u8s(16)
    d["vpm_input_size"] = r.u32()
    d["vpm_output_size"] = r.u32()
    d["driver_location_map"] = [r.i32() for _ in range(64)]
    return k, d


def fs_variant(r):
    k = key_common(r)
    for f in ("is_points", "is_lines", "line_smoothing", "point_coord_upper_left", "msaa",
              "sample_alpha_to_coverage", "sample_alpha_to_one", "can_earlyz_with_discard",
              "software_blend", "ignore_sample_mask", "cbufs", "swap_color_rb", "f32_color_rb",
              "norm_16", "snorm", "ucp_enables", "has_gs"):
        k[f] = r.u8()
    r.pad(3)
    k["color_fmt"] = [(r.u32(), r.u8s(4)) for _ in range(8)]
    k["blend"] = [r.u32s(6) for _ in range(8)]
    k["logicop_func"] = r.u32()
    k["point_sprite_mask"] = r.u32()
    d = prog_data_common(r)
    for f in ("uses_pid", "num_inputs", "writes_z", "writes_z_from_fep", "disable_ez",
              "uses_center_w", "uses_implicit_point_line_varyings",
              "lock_scoreboard_on_first_thrsw", "force_per_sample_msaa"):
        d[f] = r.u8()
    r.pad(3)
    d["input_slots"] = r.u8s(64)[:d["num_inputs"]]
    d["flat_shade_flags"] = r.u32s(3)
    d["noperspective_flags"] = r.u32s(3)
    d["centroid_flags"] = r.u32s(3)
    return k, d


def nonzero(dic):
    """Fields worth printing: drop zeros and all-zero arrays."""
    out = []
    for k, v in dic.items():
        if isinstance(v, list):
            if any(x not in (0, (0, [0, 0, 0, 0]), [0] * 6, -1) for x in v):
                out.append("%s=%s" % (k, v))
        elif v:
            out.append("%s=%s" % (k, v))
    return " ".join(out)


def dump(path, show_qpu):
    data = open(path, "rb").read()
    if len(data) < 32:
        raise Bad("shorter than the header")
    magic, major, minor, ver, commit, nsec, crc = struct.unpack("<4sHHI12sII", data[:32])
    if magic != b"V3DP":
        raise Bad("bad magic %r" % magic)
    if zlib.crc32(data[32:]) != crc:
        raise Bad("CRC mismatch: header %08x, data %08x" % (crc, zlib.crc32(data[32:])))
    print("%s: format %d.%d, V3D %d, Mesa %s, %d sections, %d bytes" %
          (path, major, minor, ver, commit.rstrip(b"\0").decode(), nsec, len(data)))
    if major != 1:
        raise Bad("unknown major version %d" % major)

    secs, o = [], 32
    while o < len(data):
        if o + 8 > len(data):
            raise Bad("truncated section header at %d" % o)
        t, n = struct.unpack("<II", data[o:o + 8])
        if o + 8 + n > len(data):
            raise Bad("section at %d runs past the end" % o)
        secs.append((t, data[o + 8:o + 8 + n]))
        padded = (n + 3) & ~3
        if any(data[o + 8 + n:o + 8 + padded]):
            raise Bad("nonzero section padding at %d" % (o + 8 + n))
        o += 8 + padded
    if len(secs) != nsec:
        raise Bad("header says %d sections, found %d" % (nsec, len(secs)))
    if [t for t, _ in secs[:2]] != [1, 2]:
        raise Bad("PROGRAM and STRINGS must come first")
    for t in (4, 5):
        if sum(1 for s, _ in secs if s == t) != 1:
            raise Bad("need exactly one %s section" % SECTIONS[t])

    strings = secs[1][1]
    if not strings or strings[0] != 0 or strings[-1] != 0:
        raise Bad("string table must start and end with NUL")

    for t, payload in secs:
        name = SECTIONS.get(t, "unknown %d" % t)
        r = Reader(payload, name)
        if t == 1:
            h = r.take(32).hex()
            mask = r.u32()
            r.done()
            print("PROGRAM hash %s, stages %s" %
                  (h, ", ".join(STAGES[s] for s in STAGES if mask & (1 << s))))
        elif t == 2:
            print("STRINGS %d bytes" % len(payload))
        elif t == 3:
            stage, nvals, nparams = r.u32(), r.u32(), r.u32()
            print("STAGE_PARAMS %s: %d values, %d parameters" %
                  (STAGES.get(stage, stage), nvals, nparams))
            params = []
            for _ in range(nparams):
                kind, nm, off, size, dtype = r.u32(), r.u32(), r.u32(), r.u32(), r.u32()
                st = [r.i16() for _ in range(4)]
                if kind not in KINDS:
                    raise Bad("parameter kind %d" % kind)
                if off + size > nvals:
                    raise Bad("parameter %s past the constant buffer" % string(strings, nm))
                params.append((kind, string(strings, nm), off, size, dtype, st))
            vals = r.u32s(nvals)
            r.done()
            for kind, nm, off, size, dtype, st in params:
                extra = ""
                if kind == 1:
                    extra = " = " + " ".join("%g" % struct.unpack("<f", struct.pack("<I", v))[0]
                                             for v in vals[off:off + size])
                elif kind == 2:
                    extra = " state %s" % st
                print("  [%3d..%3d) %-8s %-24s type 0x%04x%s" %
                      (off, off + size, KINDS[kind], nm or "-", dtype, extra))
        elif t == 4:
            n = r.u32()
            print("UNIFORMS %d" % n)
            for _ in range(n):
                nm, gtype, arr, flags, nst = r.u32(), r.u32(), r.u32(), r.u32(), r.u32()
                places = []
                for _ in range(nst):
                    stage, off = r.u32(), r.u32()
                    es, vs, fmt = r.u8(), r.u8(), r.u8()
                    r.pad(1)
                    if fmt > 1:
                        raise Bad("driver format %d" % fmt)
                    places.append("%s@%d es=%d vs=%d%s" % (STAGES.get(stage, stage), off, es, vs,
                                                          " int-as-float" if fmt else ""))
                samp = r.u32s(2)
                fl = [f for b, f in ((1, "sampler"), (2, "builtin"), (4, "hidden")) if flags & b]
                print("  %-24s type 0x%04x%s %s %s%s" %
                      (string(strings, nm), gtype, "[%d]" % arr if arr else "", ",".join(fl),
                       " ".join(places),
                       " units vs=%s fs=%s" % tuple("-" if x == 0xffffffff else x for x in samp)
                       if flags & 1 else ""))
            r.done()
        elif t == 5:
            n = r.u32()
            print("ATTRIBUTES %d" % n)
            for _ in range(n):
                nm, gtype, loc, slot = r.u32(), r.u32(), r.u32(), r.u32()
                print("  %-24s type 0x%04x location %d slot %s" %
                      (string(strings, nm), gtype, loc, "-" if slot == 0xffffffff else slot))
            r.done()
        elif t == 6:
            stage = r.u32()
            if stage in (0, 1):
                k, d = vs_variant(r)
            elif stage == 4:
                k, d = fs_variant(r)
            else:
                raise Bad("variant stage %d" % stage)
            n = r.u32()
            contents, words = r.u32s(n), r.u32s(n)
            qsize = r.u32()
            if qsize % 8:
                raise Bad("QPU size %d not a multiple of 8" % qsize)
            qpu = [r.u64() for _ in range(qsize // 8)]
            r.done()
            print("VARIANT %s: %d instructions, %d uniforms" %
                  (VARIANT_STAGES[stage], len(qpu), n))
            print("  key: " + nonzero(k))
            print("  prog data: " + nonzero(d))
            print("  uniforms: " + " ".join(recipe(c, w) for c, w in zip(contents, words)))
            if show_qpu:
                for i, q in enumerate(qpu):
                    print("  %4d %016x" % (i, q))
        else:
            print("%s %d bytes (skipped)" % (name, len(payload)))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__.strip().splitlines()[-1])
        return 2
    try:
        dump(args[0], "--qpu" in sys.argv)
    except Bad as e:
        print("v3dp_dump: %s: %s" % (args[0], e), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
