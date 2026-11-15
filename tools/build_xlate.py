"""Build the shader translator: build/xlate/totk_xlate.exe (Ryujinx + patches, NativeAOT, SPIR-V for Vulkan).

Adapted from dkb-pc-recomp (MIT). Copies tools/vendor/ryujinx to build/xlate/ryujinx,
applies tools/xlate/patches there, then runs `dotnet publish` with NativeAOT.
"""

import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / "tools" / "vendor" / "ryujinx"
XLATE = ROOT / "tools" / "xlate"
BUILD = ROOT / "build" / "xlate"
WIN = os.name == "nt"
EXE = "totk_xlate.exe" if WIN else "totk_xlate"
RID = "win-x64" if WIN else ("osx-x64" if platform.machine() == "x86_64" else "osx-arm64") if sys.platform == "darwin" \
    else "linux-x64"
VSWHERE_DIR = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer"
DEFINED_FLOAT_MINMAX_PATCH = "0014-numeric-float-minmax-and-saturation.patch"


def tree_hashes(base: Path) -> dict[str, str]:
    def build_output(rel: Path) -> bool:
        return len(rel.parts) > 2 and rel.parts[0] == "src" and rel.parts[2] in ("obj", "bin")

    return {
        p.relative_to(base).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(base.rglob("*"))
        if p.is_file() and not build_output(p.relative_to(base))
    }


def selected_patches(*, defined_float_minmax: bool = False) -> list[Path]:
    patches = sorted((XLATE / "patches").glob("*.patch"))
    optional = XLATE / "patches" / DEFINED_FLOAT_MINMAX_PATCH
    return patches if defined_float_minmax else [p for p in patches if p != optional]


def prepare_sources(*, defined_float_minmax: bool = False) -> Path:
    src = BUILD / "ryujinx"
    if BUILD.exists():
        shutil.rmtree(BUILD / "ryujinx", ignore_errors=True)
    shutil.copytree(VENDOR, src)
    for junk in list(src.glob("src/*/obj")) + list(src.glob("src/*/bin")):
        shutil.rmtree(junk)
    for patch in selected_patches(defined_float_minmax=defined_float_minmax):
        for extra in (["--check"], []):
            r = subprocess.run(
                ["git", "apply", *extra, "--whitespace=nowarn", "--directory", str(src.relative_to(BUILD)), str(patch)],
                cwd=BUILD, capture_output=True, text=True,
            )
            if r.returncode != 0:
                sys.exit(f"patch does not apply: {patch.name}\n{r.stdout}{r.stderr}")
        print(f"applied {patch.name}")
    return src


def identity(*, defined_float_minmax: bool = False) -> str:
    h = hashlib.sha256()
    files = [Path(__file__).resolve(), XLATE / "Program.cs", XLATE / "totk_xlate.csproj",
             *selected_patches(defined_float_minmax=defined_float_minmax)]
    for f in files:
        h.update(f"{f.name}\0{hashlib.sha256(f.read_bytes()).hexdigest()}\n".encode())
    for rel, digest in tree_hashes(VENDOR).items():
        h.update(f"vendor/{rel}\0{digest}\n".encode())
    return h.hexdigest()


def write_identity(exe: Path, *, defined_float_minmax: bool = False) -> None:
    ident = identity(defined_float_minmax=defined_float_minmax)
    exe.with_suffix(".id").write_text(ident + "\n", encoding="ascii")
    print(f"translator identity {ident[:16]} -> {exe.with_suffix('.id')}")


def find_dotnet() -> str | None:
    local = ROOT / "toolchain" / "dotnet" / ("dotnet.exe" if WIN else "dotnet")
    if local.is_file():
        return str(local)
    return shutil.which("dotnet")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--id-only", action="store_true", help="write totk_xlate.id for the exe already built")
    parser.add_argument("--out", type=Path, help="build directory instead of build/xlate")
    parser.add_argument("--defined-float-minmax", action="store_true")
    parser.add_argument("--llvm", type=Path)
    parser.add_argument("--sysroot", type=Path)
    a = parser.parse_args()
    if a.out:
        global BUILD
        BUILD = a.out.resolve()
    if a.id_only:
        write_identity(BUILD / EXE, defined_float_minmax=a.defined_float_minmax)
        return
    started = time.time()
    dotnet = find_dotnet()
    if not dotnet:
        sys.exit("missing: dotnet (.NET 10 SDK)")
    env = dict(os.environ)
    if WIN:
        if not (VSWHERE_DIR / "vswhere.exe").is_file() and not shutil.which("vswhere"):
            sys.exit(f"missing: vswhere.exe (expected in {VSWHERE_DIR}); NativeAOT cannot link without it")
        env["PATH"] = str(VSWHERE_DIR) + os.pathsep + env["PATH"]

    src = prepare_sources(defined_float_minmax=a.defined_float_minmax)
    work = BUILD / "xlate"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    for name in ("Program.cs", "totk_xlate.csproj"):
        shutil.copy2(XLATE / name, work / name)
    out = BUILD / "publish"
    shutil.rmtree(out, ignore_errors=True)
    cmd = [
        dotnet, "publish", str(work / "totk_xlate.csproj"), "-c", "Release", "-r", RID,
        "-p:PublishAot=true", "-p:InvariantGlobalization=true", f"-p:RyuSrc={src}", "-o", str(out),
    ]
    if a.llvm:
        env["PATH"] = str(a.llvm.resolve() / "bin") + os.pathsep + env["PATH"]
        cmd += ["-p:CppCompilerAndLinker=clang", "-p:LinkerFlavor=lld"]
    if a.sysroot:
        cmd.append(f"-p:SysRoot={a.sysroot.resolve()}")
    if subprocess.run(cmd, env=env).returncode != 0:
        sys.exit("dotnet publish failed")
    exe = BUILD / EXE
    shutil.copy2(out / EXE, exe)
    write_identity(exe, defined_float_minmax=a.defined_float_minmax)
    print(f"built {exe} ({exe.stat().st_size / 1e6:.1f} MB) in {time.time() - started:.0f} s")


if __name__ == "__main__":
    main()
