"""Clean CMake + Ninja build script for the 1:1 MTF-Recomp fork.

Outputs everything under build/fork/:
  build/fork/gen/{main,sdk,nro}-full   lifted C from recomp/lift.py & nro_lift.py
  build/fork/glue                      modules.c & layout.h from recomp/glue.py
  build/fork/cmake                     CMake + Ninja build tree
  build/fork/bin                       mtf-recomp.exe, DLLs, shaders/
  build/fork/profile                   local save/config directory
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import time
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LLVM_ROOT = ROOT / "toolchain" / "llvm-mingw-20261104-ucrt-x86_64"
NINJA = ROOT / "toolchain" / "ninja" / "ninja.exe"
DEPS_DIR = ROOT / "toolchain" / "deps-mingw"
NRO_DEFAULT = ROOT / "build" / "nro" / "1ec347bc9f4664b467fc5f9b3a9336fe91e22e119fc7fb60f8cf44f0d30ce975.nro"
REF_DIR = Path(r"C:\Users\rafae\Saved Games\Ports\Switch\mtf-ref")


def find_cmake() -> Path:
    found = shutil.which("cmake")
    if found:
        return Path(found)
    patterns = [
        r"C:\Program Files*\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        r"C:\Program Files\CMake\bin\cmake.exe",
    ]
    for pat in patterns:
        matches = sorted(glob.glob(pat), reverse=True)
        if matches:
            return Path(matches[0])
    sys.exit("cmake.exe not found in PATH or Visual Studio")


def load_game_paths() -> tuple[Path, Path]:
    cfg_path = ROOT / "config.local.toml"
    if not cfg_path.is_file():
        sys.exit(f"Missing {cfg_path}")
    cfg = tomllib.loads(cfg_path.read_text(encoding="utf-8"))
    dump_path = Path(cfg["game"]["dump_path"])
    ver = str(cfg["game"].get("update_version", "327680"))
    exefs = dump_path / "updates" / ver / "exefs"
    romfs = dump_path / "updates" / ver / "romfs"
    if not exefs.is_dir() or not romfs.is_dir():
        sys.exit(f"Missing exefs/romfs under {dump_path / 'updates' / ver}")
    return exefs, romfs


def run(cmd: list[str], env: dict[str, str], desc: str) -> None:
    t0 = time.time()
    print(f"==> {desc}")
    print("   ", " ".join(str(c) for c in cmd))
    res = subprocess.run([str(c) for c in cmd], cwd=str(ROOT), env=env)
    if res.returncode != 0:
        sys.exit(f"FAILED ({res.returncode}): {desc}")
    print(f"    done in {time.time() - t0:.1f}s")


def has_c(folder: Path) -> bool:
    return folder.is_dir() and next(folder.rglob("*.c"), None) is not None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--clean", action="store_true", help="Remove build/fork/cmake and build/fork/glue before building")
    ap.add_argument("--relift", action="store_true", help="Re-run recomp/lift.py and recomp/nro_lift.py")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    ap.add_argument("--cpu-level", default="v3", choices=("v2", "v3"))
    args = ap.parse_args()

    cmake = find_cmake()
    if not LLVM_ROOT.is_dir():
        sys.exit(f"Missing llvm-mingw at {LLVM_ROOT}")
    if not NINJA.is_file():
        sys.exit(f"Missing ninja at {NINJA}")
    if not (DEPS_DIR / "include" / "SDL3" / "SDL.h").is_file():
        sys.exit(f"Missing deps at {DEPS_DIR}")

    exefs, romfs = load_game_paths()
    fork_dir = ROOT / "build" / "fork"
    gen_dir = ROOT / "build" / "generated"
    glue_dir = gen_dir / "glue"
    cmake_dir = fork_dir / "cmake"
    bin_dir = fork_dir / "bin"
    profile_dir = fork_dir / "profile"
    profile_dir.mkdir(parents=True, exist_ok=True)
    bin_dir.mkdir(parents=True, exist_ok=True)

    if args.clean:
        shutil.rmtree(cmake_dir, ignore_errors=True)
        shutil.rmtree(glue_dir, ignore_errors=True)

    env = dict(os.environ)
    env["MTF_LLVM_MINGW_ROOT"] = str(LLVM_ROOT)
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"

    # 1. Lift main & sdk
    for mod in ("main", "sdk"):
        out = gen_dir / f"{mod}-full"
        if args.relift or not has_c(out):
            run([
                sys.executable, str(ROOT / "recomp" / "lift.py"),
                "--exefs", str(exefs),
                "--out", str(out),
                "--modules", mod,
                "--jobs", str(args.jobs),
            ], env, f"Lift {mod}")

    # 2. Lift nro
    nro_out = gen_dir / "nro-full"
    if args.relift or not has_c(nro_out):
        if not NRO_DEFAULT.is_file():
            sys.exit(f"Missing captured NRO at {NRO_DEFAULT}")
        run([
            sys.executable, str(ROOT / "recomp" / "nro_lift.py"),
            "--exefs", str(exefs),
            "--nro", str(NRO_DEFAULT),
            "--out", str(nro_out),
            "--jobs", str(args.jobs),
        ], env, "Lift nro")

    # 3. Glue
    dirs = [gen_dir / "main-full", gen_dir / "sdk-full", nro_out]
    if args.clean or not (glue_dir / "modules.c").is_file() or not (glue_dir / "layout.h").is_file():
        run([
            sys.executable, str(ROOT / "recomp" / "glue.py"),
            "--generated", *(str(d) for d in dirs),
            "--exefs", str(exefs),
            "--romfs", str(romfs),
            "--user", str(profile_dir),
            "--out", str(glue_dir),
        ], env, "Generate glue (modules.c, layout.h)")

    # 4. CMake configure
    if not (cmake_dir / "build.ninja").is_file():
        toolchain = ROOT / "tools" / "cmake" / "llvm-mingw.cmake"
        run([
            str(cmake),
            "-S", str(ROOT),
            "-B", str(cmake_dir),
            "-G", "Ninja",
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}",
            f"-DCMAKE_CXX_COMPILER={(LLVM_ROOT / 'bin' / 'clang++.exe').as_posix()}",
            "-DCMAKE_CXX_COMPILER_TARGET=x86_64-w64-windows-gnu",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_MAKE_PROGRAM={NINJA.as_posix()}",
            f"-DMTF_GENERATED_DIRS={';'.join(d.as_posix() for d in dirs)}",
            f"-DMTF_GLUE_DIR={glue_dir.as_posix()}",
            f"-DMTF_OUTPUT_DIR={bin_dir.as_posix()}",
            f"-DMTF_DEPS_DIR={DEPS_DIR.as_posix()}",
            f"-DMTF_CPU_LEVEL={args.cpu_level}",
        ], env, "CMake configure")

    # 5. Ninja build
    run([
        str(cmake), "--build", str(cmake_dir), "--parallel", str(args.jobs)
    ], env, "CMake build (Ninja)")

    # 6. Link/sync shaders & DLSS into build/fork/bin if available
    shaders_dst = bin_dir / "shaders"
    shaders_src = REF_DIR / "shaders"
    if not shaders_dst.exists() and shaders_src.is_dir():
        print(f"==> Creating junction {shaders_dst} -> {shaders_src}")
        subprocess.run(["cmd", "/c", "mklink", "/J", str(shaders_dst), str(shaders_src)], check=True)

    dlss_dst = bin_dir / "nvngx_dlss.dll"
    dlss_src = REF_DIR / "nvngx_dlss.dll"
    if not dlss_dst.exists() and dlss_src.is_file():
        shutil.copy2(dlss_src, dlss_dst)

    winpthread_dst = bin_dir / "libwinpthread-1.dll"
    if not winpthread_dst.exists():
        for cand in (
            LLVM_ROOT / "x86_64-w64-mingw32" / "bin" / "libwinpthread-1.dll",
            REF_DIR / "libwinpthread-1.dll",
        ):
            if cand.is_file():
                shutil.copy2(cand, winpthread_dst)
                break

    print(f"\nBuild ready: {bin_dir / 'mtf-recomp.exe'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
