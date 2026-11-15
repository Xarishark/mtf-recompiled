#!/usr/bin/env python3
"""MTF Recomp — End-User Local-Build Installer & Launcher (Tkinter + CLI)."""

from __future__ import annotations

import argparse
import os
import queue
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def write_config_local(dump_path: Path, update_version: str = "327680") -> Path:
    cfg = ROOT / "config.local.toml"
    posix = dump_path.resolve().as_posix()
    cfg.write_text(
        f'[game]\ndump_path = "{posix}"\nupdate_version = "{update_version}"\n',
        encoding="utf-8",
    )
    return cfg


def build_env() -> dict[str, str]:
    env = os.environ.copy()
    extra_bins = [
        ROOT / "toolchain" / "deps-mingw" / "bin",
        ROOT / "toolchain" / "llvm-mingw-20261104-ucrt-x86_64" / "bin",
        ROOT / "toolchain" / "dotnet",
    ]
    existing = [str(p) for p in extra_bins if p.is_dir()]
    if existing:
        env["PATH"] = os.pathsep.join(existing) + os.pathsep + env.get("PATH", "")
    dotnet_dir = ROOT / "toolchain" / "dotnet"
    if dotnet_dir.is_dir():
        env["DOTNET_ROOT"] = str(dotnet_dir)
    env["MTF_GPU"] = "vulkan"
    env["TOTK_ASYNC_PREEMPT"] = "0"
    return env


def run_pipeline(dump_path: Path, update_version: str, log_fn=print) -> int:
    write_config_local(dump_path, update_version)
    log_fn(f"[setup] Wrote {ROOT / 'config.local.toml'} -> {dump_path}")
    env = build_env()
    upd_dir = dump_path / "updates" / update_version
    exefs_dir = upd_dir / "exefs"
    rom42 = upd_dir / "romfs" / "42"
    nro_path = ROOT / "build" / "nro" / "1ec347bc9f4664b467fc5f9b3a9336fe91e22e119fc7fb60f8cf44f0d30ce975.nro"
    sh_dir = ROOT / f"build/v{update_version}/shaders-nvn2"
    xlate_exe = sh_dir / "xlate" / "totk_xlate.exe"

    steps: list[tuple[str, list[str]]] = [
        ("Validate dump", [sys.executable, "tools/check_dump.py"]),
        ("Verify content guard", [sys.executable, "tools/check_content.py"]),
        ("Lift main & sdk", [sys.executable, "recomp/gen.py", "--exefs", str(exefs_dir), "--out", f"generated/v{update_version}", "--modules", "main,sdk", "--jobs", "8"]),
        ("Build Stage-1", [sys.executable, "tools/build_stage1.py", "--jobs", "8"]),
    ]

    if nro_path.is_file():
        if not (ROOT / f"generated/v{update_version}" / "nro0_tables.c").is_file():
            steps.append(("Lift nro0", [sys.executable, "recomp/gen.py", "--exefs", str(exefs_dir), "--out", f"generated/v{update_version}", "--modules", "nro0", "--nro", f"nro0={nro_path}", "--jobs", "8"]))
        if not (sh_dir / "shaders.bin").is_file():
            steps.append(("Extract shaders", [sys.executable, "tools/shaders/extract.py", "--nro", str(nro_path), "--rom42", str(rom42), "--out", str(sh_dir)]))
        if not xlate_exe.is_file():
            dotnet = ROOT / "toolchain" / "dotnet" / "dotnet.exe"
            steps.append(("Build shader translator", [str(dotnet), "publish", "tools/xlate/totk_xlate.csproj", "-c", "Release", "-o", str(xlate_exe.parent)]))
        steps.append(("Translate shaders", [sys.executable, "tools/shaders/translate.py", "--pack", str(sh_dir / "shaders.bin"), "--xlate", str(xlate_exe), "--out", str(sh_dir / "spv"), "--skip-existing"]))
    else:
        log_fn(f"[setup] Note: {nro_path} not yet captured; skipping nro0/shader steps until Stage-2 capture.")

    steps.append(("Build Stage-2", [sys.executable, "tools/build_stage2.py", "--jobs", "16"]))

    for title, cmd in steps:
        log_fn(f"\n=== {title} ===")
        proc = subprocess.Popen(
            cmd,
            cwd=str(ROOT),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            log_fn(line.rstrip())
        rc = proc.wait()
        if rc != 0:
            log_fn(f"[setup] Step failed ({rc}): {title}")
            return rc
    log_fn("\n[setup] Build complete! Ready to launch mtf-stage2.exe.")
    return 0


def launch_gui() -> int:
    import tkinter as tk
    from tkinter import filedialog, messagebox, scrolledtext

    root = tk.Tk()
    root.title("MTF Recomp — Setup & Launcher")
    root.geometry("760x520")

    dump_var = tk.StringVar()
    ver_var = tk.StringVar(value="327680")
    q: queue.Queue[str] = queue.Queue()

    frm = tk.Frame(root, padx=12, pady=10)
    frm.pack(fill=tk.BOTH, expand=True)

    tk.Label(frm, text="Game Dump Folder or .nup Package (read-only):").pack(anchor="w")
    row = tk.Frame(frm)
    row.pack(fill=tk.X, pady=(2, 8))
    tk.Entry(row, textvariable=dump_var).pack(side=tk.LEFT, fill=tk.X, expand=True)

    def pick_folder() -> None:
        p = filedialog.askdirectory(title="Select Mario Tennis Fever Dump Folder")
        if p:
            dump_var.set(p)

    def pick_nup() -> None:
        p = filedialog.askopenfilename(title="Select .nup Package", filetypes=[("NUP package", "*.nup"), ("All", "*.*")])
        if p:
            dump_var.set(p)

    tk.Button(row, text="Folder...", command=pick_folder).pack(side=tk.LEFT, padx=(6, 0))
    tk.Button(row, text=".nup...", command=pick_nup).pack(side=tk.LEFT, padx=(4, 0))

    ver_row = tk.Frame(frm)
    ver_row.pack(fill=tk.X, pady=(0, 8))
    tk.Label(ver_row, text="Update version:").pack(side=tk.LEFT)
    tk.Entry(ver_row, textvariable=ver_var, width=12).pack(side=tk.LEFT, padx=(6, 0))

    log_box = scrolledtext.ScrolledText(frm, height=20, state=tk.DISABLED)
    log_box.pack(fill=tk.BOTH, expand=True, pady=(0, 8))

    btns = tk.Frame(frm)
    btns.pack(fill=tk.X)

    def poll_queue() -> None:
        while not q.empty():
            msg = q.get_nowait()
            log_box.configure(state=tk.NORMAL)
            log_box.insert(tk.END, msg + "\n")
            log_box.see(tk.END)
            log_box.configure(state=tk.DISABLED)
        root.after(100, poll_queue)

    def on_build() -> None:
        raw = dump_var.get().strip()
        if not raw:
            messagebox.showerror("MTF Recomp", "Select a game dump folder or .nup file first.")
            return
        p = Path(raw)
        if p.is_file() and p.suffix.lower() == ".nup":
            q.put("[setup] .nup extraction via tools/vendor/nup can be invoked here; please select extracted dump or extract .nup first.")
            return
        threading.Thread(target=lambda: run_pipeline(p, ver_var.get().strip() or "327680", q.put), daemon=True).start()

    def on_launch() -> None:
        exe = ROOT / "build" / "v327680_stage2" / "mtf-stage2.exe"
        if not exe.is_file():
            messagebox.showerror("MTF Recomp", f"Executable not found:\n{exe}")
            return
        subprocess.Popen([str(exe), "--boot"], cwd=str(ROOT), env=build_env())

    tk.Button(btns, text="Validate & Build", command=on_build).pack(side=tk.LEFT)
    tk.Button(btns, text="Launch Game (Vulkan)", command=on_launch).pack(side=tk.LEFT, padx=(8, 0))
    root.after(100, poll_queue)
    root.mainloop()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="MTF Recomp Setup & Launcher")
    ap.add_argument("--dump", type=Path, help="Path to extracted game dump")
    ap.add_argument("--update", default="327680", help="Update version (default 327680)")
    ap.add_argument("--cli", action="store_true", help="Run headless CLI setup")
    args = ap.parse_args()
    if args.cli:
        if not args.dump:
            ap.error("--cli requires --dump")
        return run_pipeline(args.dump, args.update)
    return launch_gui()


if __name__ == "__main__":
    raise SystemExit(main())
