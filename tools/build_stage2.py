"""Stage-2 Horizon OS HLE build script (Phase F2).

Reuses the compiled generated objects from build/v327680_stage1/obj/, compiles the
Horizon OS HLE runtime (memory, kernel, svc, ipc, loader, boot, fs, romfs, ro, etc.),
and links build/v327680_stage2/mtf-stage2.exe to boot `main` and capture `nro0`.
"""

import argparse
import hashlib
import os
import subprocess
import sys
import time
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLANG = ROOT / "toolchain" / "llvm-mingw-20261104-ucrt-x86_64" / "bin" / "clang.exe"
CLANGXX = ROOT / "toolchain" / "llvm-mingw-20261104-ucrt-x86_64" / "bin" / "clang++.exe"
NINJA = ROOT / "toolchain" / "ninja" / "ninja.exe"

RT_FLAGS = [
    "-O2",
    "-g",
    "-Wall",
    "-fno-strict-aliasing",
    "-march=x86-64-v3",
    "-D_WIN32_WINNT=0x0A00",
    "-DMTF_STAGE2",
    f"-I{(ROOT / 'toolchain/deps-mingw/include').as_posix()}",
]

LINK_LIBS = [
    f"-L{(ROOT / 'toolchain/deps-mingw/lib').as_posix()}",
    "-lvulkan-1",
    "-lSDL3",
    "-l:libopus.a",
    "-l:libvpx.a",
    "-l:libzstd.a",
    "-lc++",
    "-lpthread",
    "-lpsapi",
    "-lbcrypt",
    "-luser32",
    "-lgdi32",
    "-ldwmapi",
    "-lwinmm",
    "-lole32",
    "-lshell32",
    "-luuid",
    "-lsetupapi",
    "-limm32",
    "-lversion",
    "-lxinput",
]

RT_SOURCES = [
    "audio_effects.c",
    "audio_mixer.c",
    "audio_reverb.c",
    "audren.c",
    "boot.c",
    "cfg.c",
    "dispatch.c",
    "excl.c",
    "flight.c",
    "fma.c",
    "frame_rate.c",
    "fs_host.c",
    "gfx_c0.c",
    "gfx_null.c",
    "gfx_shader.c",
    "gfx_spirv.c",
    "gfx_window.c",
    "gfx_window_win.c",
    "hostfn.c",
    "input.c",
    "input_map.c",
    "input_script.c",
    "input_win.c",
    "ipc.c",
    "kernel.c",
    "loader.c",
    "main.c",
    "memory.c",
    "motion_fusion.c",
    "nvn_api_cmd.c",
    "nvn_api_core.c",
    "nvn_api_nvn2.c",
    "nvn_api_obj.c",
    "nvn_ext.c",
    "nvn_mode_trace.c",
    "nvn_procs.c",
    "nvn_tex.c",
    "ov_codec.c",
    "ov_gfx.c",
    "ov_input.c",
    "ov_movie.c",
    "ov_resolution.c",
    "ov_system.c",
    "amiibo.c",
    "patches.c",
    "perf.c",
    "perfmon.c",
    "rdoc.c",
    "ro.c",
    "romfs.c",
    "sha256.c",
    "spirv_hdr_shoulder.c",
    "spirv_lq_fold.cpp",
    "spirv_nan_guard.c",
    "spirv_rewrite.c",
    "spirv_screen_uv.c",
    "stage2_stubs.c",
    "svc.c",
    "sysinfo.c",
    "tex_astc.c",
    "tex_astc_mt.c",
    "tex_deswizzle.c",
    "ui_text.c",
    "ui/binds_page.cpp",
    "ui/menu.cpp",
    "ui/menu_win.cpp",
    "ui/option_rows.cpp",
    "third_party/imgui/imgui.cpp",
    "third_party/imgui/imgui_draw.cpp",
    "third_party/imgui/imgui_tables.cpp",
    "third_party/imgui/imgui_widgets.cpp",
    "third_party/imgui/backends/imgui_impl_vulkan.cpp",
    "vk_astc.c",
    "vk_backend.c",
    "vk_caps.c",
    "vk_clear.c",
    "vk_color.c",
    "vk_crumbs.c",
    "vk_device.c",
    "vk_dlaa.c",
    "vk_dump.c",
    "vk_exec.c",
    "vk_exec_draw.c",
    "vk_exec_draw_helper.c",
    "vk_exec_xfer.c",
    "vk_formats.c",
    "vk_fsr.c",
    "vk_hdr.c",
    "vk_headroom.c",
    "vk_memory.c",
    "vk_pipeline.c",
    "vk_present.c",
    "vk_rec.c",
    "vk_smaa.c",
    "vk_spec.c",
    "vk_sync.c",
    "vk_texture.c",
    "vk_usnap.c",
    "vk_vma.cpp",
    "platform/common.c",
    "platform/win32.c",
]


def npath(p: Path) -> str:
    return p.resolve().as_posix().replace(":", "$:")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def load_dump_dir() -> Path:
    cfg_path = ROOT / "config.local.toml"
    if not cfg_path.is_file():
        sys.exit("Missing config.local.toml")
    cfg = tomllib.loads(cfg_path.read_text(encoding="utf-8"))
    return Path(cfg["game"]["dump_path"]) / "updates" / str(cfg["game"]["update_version"])


def write_modules_c(out_path: Path, upd_dir: Path, gen_dir: Path) -> None:
    exefs_dir = (upd_dir / "exefs").resolve().as_posix()
    romfs_dir = (upd_dir / "romfs").resolve().as_posix()
    profile_dir = (ROOT / "build" / "v327680_stage2" / "profile").resolve().as_posix()
    port_ver = (ROOT / "VERSION").read_text(encoding="utf-8").strip()

    exefs_hashes = []
    for name in ("main", "sdk", "rtld", "main.npdm"):
        p = upd_dir / "exefs" / name
        if p.is_file():
            exefs_hashes.append((name, sha256_file(p)))
    nro_files = sorted((ROOT / "build" / "nro").glob("*.nro"))
    if nro_files:
        exefs_hashes.append(("nro0", nro_files[0].stem))

    lines = [
        '#include "dispatch.h"',
        '#include "host.h"',
        '#include "layout.h"',
        "",
        "#define DECL(name, NAME) \\",
        "    extern const uint64_t name##_fn_addrs[]; \\",
        "    extern const GuestFn name##_fn_ptrs[]; \\",
        "    extern const uint64_t name##_fn_count;",
        "MOD_LIST(DECL)",
        "#undef DECL",
        "",
        "const RtLayoutEntry rt_layout[] = {",
        "#define ENTRY(name, NAME) {#name, MOD_##NAME##_BASE, MOD_##NAME##_SIZE},",
        "    MOD_LIST(ENTRY)",
        "#undef ENTRY",
        "};",
        "const int rt_layout_count = sizeof rt_layout / sizeof *rt_layout;",
        "",
        "int rt_linked_modules(RtModule *out, int max) {",
        "    const RtModule mods[] = {",
        "#define MOD(name, NAME) {#name, MOD_##NAME##_BASE, MOD_##NAME##_SIZE, name##_fn_addrs, name##_fn_ptrs, name##_fn_count},",
        "        MOD_LIST(MOD)",
        "#undef MOD",
        "    };",
        "    int n = (int)(sizeof mods / sizeof *mods);",
        "    if (n > max) n = max;",
        "    for (int i = 0; i < n; i++) out[i] = mods[i];",
        "    return n;",
        "}",
        "",
        "const RtFileHash rt_exefs_sha256[] = {",
    ]
    for name, h in exefs_hashes:
        lines.append(f'    {{"{name}", "{h}"}},')
    lines += [
        "};",
        f"const int rt_exefs_sha256_count = {len(exefs_hashes)};",
        f'const char rt_default_exefs[] = "{exefs_dir}";',
        f'const char rt_default_romfs[] = "{romfs_dir}";',
        f'const char rt_default_profile[] = "{profile_dir}";',
        "const int rt_game_version = 0;",
        'const char rt_game_target[] = "mtf327680";',
        'const char rt_game_display[] = "1.1.1";',
        f'const char rt_port_version[] = "{port_ver}";',
        "",
    ]
    out_path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gen", default=str(ROOT / "generated" / "v327680"))
    parser.add_argument("--stage1-obj", default=str(ROOT / "build" / "v327680_stage1" / "obj"))
    parser.add_argument("--out", default=str(ROOT / "build" / "v327680_stage2"))
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    args = parser.parse_args()

    gen_dir = Path(args.gen)
    s1_obj = Path(args.stage1_obj)
    out_dir = Path(args.out)
    obj_dir = out_dir / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "profile").mkdir(parents=True, exist_ok=True)

    upd_dir = load_dump_dir()
    modules_c = out_dir / "modules.c"
    write_modules_c(modules_c, upd_dir, gen_dir)

    gen_objs = sorted(
        p for p in s1_obj.glob("*.o") if p.stem not in ("fma", "stage1_stub")
    )
    if not gen_objs:
        sys.exit(f"No stage1 generated objects found in {s1_obj}")

    inc_rt = (ROOT / "runtime").resolve().as_posix()
    inc_gen = gen_dir.resolve().as_posix()
    ninja_lines = [
        f"builddir = {npath(out_dir)}",
        f"cc = {CLANG.resolve().as_posix()}",
        f"cxx = {CLANGXX.resolve().as_posix()}",
        f'rtflags = {" ".join(RT_FLAGS)} -I"{inc_rt}" -I"{inc_rt}/third_party/imgui" -I"{inc_gen}"',
        "",
        "rule cc_rt",
        "  command = $cc $rtflags -MMD -MF $out.d -c $in -o $out",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = CC(rt) $out",
        "",
        "rule cxx_rt",
        "  command = $cxx -std=c++20 $rtflags -MMD -MF $out.d -c $in -o $out",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = CXX(rt) $out",
        "",
        "rule cc_gen",
        f'  command = $cc -O2 -fno-strict-aliasing -march=x86-64-v3 -I"{inc_rt}" -I"{inc_gen}" -c $in -o $out',
        "  description = CC(gen) $out",
        "",
        "rule link",
        f'  command = $cc -fuse-ld=lld -Wl,--stack,16777216 -o $out @$out.rsp {" ".join(LINK_LIBS)}',
        "  rspfile = $out.rsp",
        "  rspfile_content = $in",
        "  description = LINK $out",
        "",
    ]

    rt_objs = []
    for rel in RT_SOURCES:
        src = ROOT / "runtime" / rel
        obj = obj_dir / (rel.replace("/", "_").replace("\\", "_") + ".o")
        rt_objs.append(obj)
        rule = "cxx_rt" if src.suffix == ".cpp" else "cc_rt"
        ninja_lines.append(f"build {npath(obj)}: {rule} {npath(src)}")

    mod_obj = obj_dir / "modules.o"
    rt_objs.append(mod_obj)
    ninja_lines.append(f"build {npath(mod_obj)}: cc_rt {npath(modules_c)}")

    nro_objs = []
    nro_dir = gen_dir / "nro0"
    if (nro_dir / "manifest.json").is_file():
        nro_obj_dir = obj_dir / "nro0"
        nro_obj_dir.mkdir(parents=True, exist_ok=True)
        for src in sorted(nro_dir.glob("*.c")):
            obj = nro_obj_dir / (src.stem + ".o")
            nro_objs.append(obj)
            ninja_lines.append(f"build {npath(obj)}: cc_gen {npath(src)}")

    exe = out_dir / "mtf-stage2.exe"
    all_objs = " ".join(npath(o) for o in (gen_objs + nro_objs + rt_objs))
    ninja_lines.append("")
    ninja_lines.append(f"build {npath(exe)}: link {all_objs}")
    ninja_lines.append(f"default {npath(exe)}")
    ninja_lines.append("")

    ninja_file = out_dir / "build.ninja"
    ninja_file.write_text("\n".join(ninja_lines), encoding="utf-8")

    t0 = time.perf_counter()
    subprocess.check_call([str(NINJA), "-f", str(ninja_file), "-j", str(args.jobs)])
    for dll_dir in (ROOT / "toolchain" / "deps-mingw" / "bin", CLANG.parent):
        if dll_dir.is_dir():
            for dll in dll_dir.glob("*.dll"):
                dst = out_dir / dll.name
                if not dst.is_file():
                    import shutil
                    shutil.copy2(dll, dst)
    dt = time.perf_counter() - t0
    print(f"Built {exe} in {dt:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
