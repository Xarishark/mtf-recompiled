#!/usr/bin/env python3
"""Package a clean, game-free MTF Recomp source/launcher release archive."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

INCLUDE_DIRS = [
    "docs",
    "installer",
    "profiles",
    "recomp",
    "runtime",
    "tests",
    "tools",
]

INCLUDE_FILES = [
    "AGENTS.md",
    "LICENSE",
    "README.md",
    "THIRD_PARTY_NOTICES.md",
    "CMakeLists.txt",
    "VERSION",
    "config.example.toml",
    "recomplauncher.json",
]

SKIP_PARTS = {
    "__pycache__",
    ".git",
    "build",
    "generated",
    "work",
    "bin",
    "obj",
}


def should_skip(path: Path) -> bool:
    if any(part in SKIP_PARTS for part in path.parts):
        return True
    if path.suffix.lower() in {".exe", ".dll", ".o", ".obj", ".spv", ".nro", ".nso", ".sqlite"}:
        return True
    return False


def make_release(version: str, out_dir: Path) -> Path:
    stage = out_dir / f"mtf-recomp-{version}-src"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True, exist_ok=True)

    for rel in INCLUDE_FILES:
        src = ROOT / rel
        if src.is_file():
            shutil.copy2(src, stage / rel)

    for rel in INCLUDE_DIRS:
        src_dir = ROOT / rel
        if not src_dir.is_dir():
            continue
        for p in src_dir.rglob("*"):
            if p.is_dir():
                continue
            rel_p = p.relative_to(ROOT)
            if should_skip(rel_p):
                continue
            dst = stage / rel_p
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst)

    # Verify zero game content inside staged directory using tools/check_content.py
    subprocess.check_call([sys.executable, str(ROOT / "tools" / "check_content.py")], cwd=str(stage))

    zip_path = out_dir / f"mtf-recomp-{version}-src.zip"
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, strict_timestamps=False) as zf:
        for p in sorted(stage.rglob("*")):
            if p.is_file():
                zf.write(p, p.relative_to(out_dir))
    return zip_path


def make_windows_zip(version: str, out_dir: Path, setup_exe: Path) -> Path:
    """Package MTF-Recomp-v<version>-Windows-x64.zip for RecompLauncher (recomplauncher.json + Setup.exe + sources)."""
    src_zip = make_release(version, out_dir)
    stage = out_dir / f"mtf-recomp-{version}-src"
    win_zip = out_dir / f"MTF-Recomp-v{version}-Windows-x64.zip"
    top = Path(f"MTF-Recomp-v{version}-Windows-x64")
    if win_zip.exists():
        win_zip.unlink()
    with zipfile.ZipFile(win_zip, "w", compression=zipfile.ZIP_DEFLATED, strict_timestamps=False) as zf:
        for p in sorted(stage.rglob("*")):
            if p.is_file():
                zf.write(p, top / p.relative_to(stage))
        if setup_exe.is_file():
            zf.write(setup_exe, top / setup_exe.name)
    return win_zip


def main() -> int:
    ap = argparse.ArgumentParser(description="Create a clean MTF Recomp release archive")
    ap.add_argument("--version", default="0.1.0-dev", help="Release version tag")
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "release", help="Output directory")
    ap.add_argument("--windows-zip", action="store_true", help="Also build MTF-Recomp-v<version>-Windows-x64.zip")
    args = ap.parse_args()
    zip_path = make_release(args.version, args.out)
    size_mb = zip_path.stat().st_size / (1024 * 1024)
    print(f"Created clean release archive: {zip_path} ({size_mb:.2f} MiB)")
    if args.windows_zip:
        win_zip = make_windows_zip(args.version, args.out, args.out / "MTF-Recomp-Setup.exe")
        win_mb = win_zip.stat().st_size / (1024 * 1024)
        print(f"Created RecompLauncher Windows archive: {win_zip} ({win_mb:.2f} MiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
