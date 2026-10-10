#!/usr/bin/env python3
"""Build Mesa's GL + V3D compiler path for m68k (the S2 compiler module and the
68k v3dglslc): replay the host Mesa build's own compile commands with the
m68k-amigaos GCC 16.2 cross-compiler.

Reads compile_commands.json of the host Mesa build (same commit, patches/
applied), rewrites each command for m68k-amigaos + libnix, compiles in
parallel and writes:
  <out>/results.json   per-file status and first error lines
  <out>/obj/...        objects that compiled
Usage: replay.py [--out DIR] [--jobs N] [--libs a,b,...|tool|all] [--extra-flags "..."]
The default is --libs tool: everything v3dglslc links. "all" adds Mesa's
standalone GLSL compiler (the stream C spike's part 1, needs patch 0005).
"""
import argparse, collections, json, os, re, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

MESA_BUILD = os.path.expanduser("~/mesa-build/mesa/build")
# Toolchain root. Default: GCC 16.2 (amiga16.2). M68K_TC=~/amiga-gcc16-install selects
# GCC 16.1 (amiga16.1), which miscompiles (see README.md) and is kept only for comparison.
TC_ROOT = os.path.expanduser(os.environ.get("M68K_TC", "~/amiga-gcc162-install"))
GCC16 = os.path.join(TC_ROOT, "bin", "m68k-amigaos-")

# Meson target directories, in dependency order.
LIBS = collections.OrderedDict([
    ("util",     "src/util/libmesa_util.a.p"),
    ("c11",      "src/c11/impl/libmesa_util_c11.a.p"),
    ("blake3",   "src/util/blake3/libblake3.a.p"),
    ("compiler", "src/compiler/libcompiler.a.p"),
    ("nir",      "src/compiler/nir/libnir.a.p"),
    ("glcpp",    "src/compiler/glsl/glcpp/libglcpp.a.p"),
    ("glsl",     "src/compiler/glsl/libglsl.a.p"),
    ("v3d_compiler", "src/broadcom/compiler/libbroadcom_compiler.a.p"),
    ("v3d_qpu",  "src/broadcom/qpu/libbroadcom_qpu.a.p"),
    ("v3d_common", "src/broadcom/libbroadcom_v3d.a.p"),
    # Mesa's standalone GLSL compiler tool: a link target that needs no GL driver.
    ("glsl_util", "src/compiler/glsl/libglsl_util.a.p"),
    ("glsl_standalone", "src/compiler/glsl/libglsl_standalone.a.p"),
    ("glsl_compiler_main", "src/compiler/glsl/glsl_compiler.p"),
    # Part 2: the driver path the offline compiler uses (GL + state tracker + gallium v3d).
    ("mesa", "src/mesa/libmesa.a.p"),
    ("glapi", "src/mesa/glapi/shared-glapi/libglapi.a.p"),
    ("gallium_aux", "src/gallium/auxiliary/libgallium.a.p"),
    ("v3d_driver", "src/gallium/drivers/v3d/libv3d.a.p"),
    ("v3d_driver42", "src/gallium/drivers/v3d/libv3d-v42.a.p"),
    ("v3d_driver71", "src/gallium/drivers/v3d/libv3d-v71.a.p"),
    ("v3d_winsys", "src/gallium/winsys/v3d/drm/libv3dwinsys.a.p"),
    ("cle", "src/broadcom/cle/libbroadcom_cle.a.p"),
    ("bc42", "src/broadcom/libbroadcom-v42.a.p"),
    ("bc71", "src/broadcom/libbroadcom-v71.a.p"),
    ("perfcntrs", "src/broadcom/perfcntrs/libbroadcom_perfcntrs.a.p"),
    ("perfcntrs42", "src/broadcom/perfcntrs/libv3d-perfcntrs-v42.a.p"),
    ("perfcntrs71", "src/broadcom/perfcntrs/libv3d-perfcntrs-v71.a.p"),
    ("xmlconfig", "src/util/libxmlconfig.a.p"),
    ("v3d_tiling", "src/broadcom/libv3d_neon.a.p"),     # v3d_tiling.c (NEON on ARM, plain C here)
    ("vtn", "src/compiler/spirv/libvtn.a.p"),           # SPIR-V front end, referenced by GL core
])
PART2 = ["mesa", "glapi", "gallium_aux", "v3d_driver", "v3d_driver42", "v3d_driver71", "v3d_winsys",
         "cle", "bc42", "bc71", "perfcntrs", "perfcntrs42", "perfcntrs71", "xmlconfig", "v3d_tiling", "vtn"]
PART1 = ["util", "c11", "blake3", "compiler", "nir", "glcpp", "glsl", "v3d_compiler", "v3d_qpu",
         "v3d_common", "glsl_util", "glsl_standalone", "glsl_compiler_main"]
# What v3dglslc links: everything but the standalone GLSL compiler.
TOOL = [l for l in PART1 if l not in ("glsl_standalone", "glsl_compiler_main")] + PART2

# Defines that describe the build host or features the compiler module does not use.
DROP_DEFINE = re.compile(
    r"^-D(HAVE_(?!__BUILTIN_|FUNC_ATTRIBUTE_(?:COLD|CONST|FLATTEN|FORMAT|MALLOC|NORETURN|"
    r"OPTIMIZE|PACKED|PURE|RETURNS_NONNULL|UNUSED|WARN_UNUSED_RESULT)\b|OPENGL)"
    r"|ALLOW_KCMP|AMD_LLVM|DRAW_LLVM|GALLIVM|LLVM_|MESA_LLVM|ENABLE_SHADER_CACHE|USE_SSE41|USE_LIBELF"
    r"|MAJOR_IN_SYSMACROS|HAS_SCHED|MESA_SYSTEM_HAS_KMS_DRM|USE_LIBGLVND|GLAPI_EXPORT|VIDEO_CODEC"
    r"|_GLIBCXX_ASSERTIONS|_FILE_OFFSET_BITS|THREAD_SANITIZER)")
DROP_EXACT = {"-fPIC", "-pthread", "-g", "-fvisibility=hidden", "-ffunction-sections",
              "-fdata-sections", "-Winvalid-pch", "-mtls-dialect=gnu2"}
DROP_PREFIX = ("-fdiagnostics-color", "-Werror", "-I/usr/include")

# Software floating point. Constant folding must round every float operation to
# single precision as x86-64 SSE does, or folded constants differ by an ulp from the
# offline compiler. 68020+68881 code keeps 80-bit intermediates (FLT_EVAL_METHOD 2);
# 68040 code (fsadd, fsmul) would round per operation on a real 68040, but Emu68
# ignores those instructions' precision and computes in double (src/M68k_LINEF.c,
# "(void)precision"), which the A1200 run showed. libgcc's soft-float is exact IEEE.
# -fno-builtin-sqrt*: keep square roots in libm (and vamos mis-emulates fssqrt).
TARGET = ["-mcpu=68020", "-msoft-float", "-mcrt=nix20", "-fno-diagnostics-color",
          "-fno-builtin-sqrt", "-fno-builtin-sqrtf"]
TARGET_CXX = ["-fno-exceptions", "-fno-rtti"]
# Kept on purpose: compiler builtins and GCC atomics exist for m68k (libatomic covers 64-bit).
# u_endian.h knows no AmigaOS branch, so the byte order is given here; libnix defines timespec.
COMPAT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "compat")
# Mesa assumes uint32_t is unsigned int; newlib on m68k-amigaos makes it unsigned long.
# Both are 32-bit here, so redefining the type changes no layout or calling convention.
ADD = ["-DUSE_GCC_ATOMIC_BUILTINS", "-DHAVE_PTHREAD", "-DMESA_DEBUG=0",
       "-DUTIL_ARCH_LITTLE_ENDIAN=0", "-DUTIL_ARCH_BIG_ENDIAN=1", "-DHAVE_STRUCT_TIMESPEC",
       "-U__INT32_TYPE__", "-D__INT32_TYPE__=int", "-U__UINT32_TYPE__", "-D__UINT32_TYPE__=unsigned int",
       "-I" + COMPAT, "-include", os.path.join(COMPAT, "amiga_compat.h")]

# Not built, with the reason. "port" files get an AmigaOS implementation instead.
EXCLUDE = {
    "src/util/texcompress_astc_luts.cpp": "ASTC texture decoding, not used by the compiler (needs std::mutex)",
    "src/util/texcompress_astc_luts_wrap.cpp": "ASTC texture decoding, not used by the compiler",
    "src/util/u_sync_provider.c": "DRM sync objects",
    "src/util/mesa_cache_db.c": "on-disk shader cache database (ogles2 has its own .v3dp cache)",
    "src/util/os_time.c": "port: needs an AmigaOS branch",
    "src/util/os_misc.c": "port: needs an AmigaOS branch",
    # Part 2 (driver path)
    "src/gallium/auxiliary/gallivm/lp_bld_debug.cpp": "LLVM JIT (llvmpipe/draw), not used by V3D",
    "src/gallium/auxiliary/gallivm/lp_bld_intr.c": "LLVM JIT",
    "src/gallium/auxiliary/gallivm/lp_bld_misc.cpp": "LLVM JIT",
    "src/gallium/auxiliary/draw/draw_llvm.c": "LLVM draw path",
    "src/gallium/auxiliary/draw/draw_pt_fetch_shade_pipeline_llvm.c": "LLVM draw path",
    "src/gallium/auxiliary/draw/draw_vs_llvm.c": "LLVM draw path",
    "src/gallium/auxiliary/nir/nir_to_tgsi_info.c": "only built with LLVM draw (header provides an inline stub)",
    "src/gallium/auxiliary/renderonly/renderonly.c": "KMS display buffers",
    "src/broadcom/cle/v3d_decoder.c": "control-list decoder for debug dumps (needs expat)",
}
PER_FILE = {
    # driconf without the XML parser: built-in defaults only.
    "src/util/xmlconfig.c": ["-UWITH_XMLCONFIG", "-DWITH_XMLCONFIG=0"],
}
# GCC 16.1 m68k: internal compiler error in reload at -O2 (GCC 16.2 is fine).
if "gcc16-install" in TC_ROOT:
    PER_FILE["src/compiler/nir/nir_validate.c"] = ["-O1"]
MESA_SRC = os.path.dirname(MESA_BUILD)


def rewrite(entry, out_dir, extra):
    args = shlex.split(entry["command"])
    is_cxx = args[0].endswith("++")
    src = entry["file"]
    if src.endswith((".S", ".s", ".asm")):
        return None
    new = [GCC16 + ("g++" if is_cxx else "gcc")]
    skip_next = False
    for i, a in enumerate(args[1:], 1):
        if skip_next:
            skip_next = False
            continue
        if a in ("-o", "-MF", "-MQ"):
            skip_next = True
            continue
        if a in ("-MD", "-c") or a == src:
            continue
        if a in DROP_EXACT or a.startswith(DROP_PREFIX) or DROP_DEFINE.match(a) or "IA32" in a:
            continue
        new.append(a)
    obj = os.path.join(out_dir, "obj", entry["output"])
    rel = os.path.normpath(os.path.join(MESA_BUILD, src)).split("/mesa/", 1)[-1]
    rel = rel.replace("build/", "", 1) if rel.startswith("build/") else rel
    new += TARGET + (TARGET_CXX if is_cxx else []) + ADD + extra + PER_FILE.get(rel, []) + ["-c", src, "-o", obj]
    return new, obj, rel


def run(job):
    lib, entry, cmd, obj = job
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    p = subprocess.run(cmd, cwd=MESA_BUILD, capture_output=True, text=True)
    errs = [l for l in p.stderr.splitlines() if " error: " in l or "fatal error:" in l]
    return {"lib": lib, "file": entry["file"], "ok": p.returncode == 0 and os.path.exists(obj),
            "obj": obj if p.returncode == 0 else None, "errors": errs[:8],
            "cmd": " ".join(shlex.quote(c) for c in cmd) if p.returncode else None}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.expanduser(os.environ.get("OUT", "~/ogles2-build/v3dglslc")))
    ap.add_argument("--jobs", type=int, default=24)
    ap.add_argument("--libs", default="tool", help="comma list, \"tool\" or \"all\"")
    ap.add_argument("--extra-flags", default="")
    ap.add_argument("--asm", action="store_true",
                    help="emit assembly (-S) instead of objects, for check_saves_asm.py")
    a = ap.parse_args()
    if a.asm:
        TARGET.append("-S")
    cc = json.load(open(os.path.join(MESA_BUILD, "compile_commands.json")))
    want = (list(LIBS) if a.libs == "all" else TOOL if a.libs == "tool"
            else [l for l in a.libs.split(",") if l])
    jobs, excluded = [], []
    for e in cc:
        for lib in want:
            if e.get("output", "").startswith(LIBS[lib] + "/"):
                r = rewrite(e, a.out, shlex.split(a.extra_flags))
                if not r:
                    continue
                if r[2] in EXCLUDE:
                    excluded.append({"lib": lib, "file": r[2], "reason": EXCLUDE[r[2]]})
                    continue
                jobs.append((lib, e, r[0], r[1]))
    # Our compatibility code, built like the Mesa C files.
    for name in sorted(os.listdir(COMPAT)):
        if not name.endswith(".c"):
            continue
        compat_src = os.path.join(COMPAT, name)
        compat_obj = os.path.join(a.out, "obj", "compat", name[:-2] + ".o")
        jobs.append(("compat", {"file": compat_src},
                     [GCC16 + "gcc", "-std=gnu11", "-O2", "-Wall", "-D_GNU_SOURCE",
                      "-I" + os.path.join(MESA_SRC, "include")] + TARGET + ADD +
                     ["-c", compat_src, "-o", compat_obj], compat_obj))
    with ThreadPoolExecutor(a.jobs) as ex:
        res = list(ex.map(run, jobs))
    json.dump(excluded, open(os.path.join(a.out, "excluded.json"), "w"), indent=1)
    want = want + ["compat"]
    json.dump(res, open(os.path.join(a.out, "results.json"), "w"), indent=1)
    by = collections.OrderedDict((l, [0, 0]) for l in want)
    for r in res:
        by[r["lib"]][0 if r["ok"] else 1] += 1
    for l, (ok, bad) in by.items():
        print(f"{l:14s} ok {ok:4d}  failed {bad:4d}")
    print(f"TOTAL          ok {sum(v[0] for v in by.values()):4d}  failed {sum(v[1] for v in by.values()):4d}")


if __name__ == "__main__":
    main()
