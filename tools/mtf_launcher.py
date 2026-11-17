#!/usr/bin/env python3
"""MTF-Recomp Launcher — Entry point GUI for Mario Tennis Fever static recompilation."""

from __future__ import annotations

import os
import queue
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, scrolledtext, ttk


def find_repo_root() -> Path:
    """Resolve the MTF-Recomp repository root whether running as script or frozen exe."""
    if getattr(sys, "frozen", False):
        exe_dir = Path(sys.executable).resolve().parent
        # When placed in build/ or repo root, walk up until tools/build_stage2.py is found.
        for candidate in (exe_dir, exe_dir.parent, exe_dir.parent.parent):
            if (candidate / "tools" / "build_stage2.py").is_file():
                return candidate
        return Path.cwd()
    return Path(__file__).resolve().parent.parent


REPO_ROOT = find_repo_root()
CONFIG_PATH = REPO_ROOT / "config.local.toml"
STAGE2_DIR = REPO_ROOT / "build" / "v327680_stage2"
STAGE2_EXE = STAGE2_DIR / "mtf-stage2.exe"
DLSS_DLL_NAME = "nvngx_dlss.dll"
KNOWN_DLSS_SOURCES = [
    Path(r"C:\Users\rafae\Saved Games\Ports\Switch\Mario Tennis") / DLSS_DLL_NAME,
    REPO_ROOT / "bin" / DLSS_DLL_NAME,
]


def read_dump_path() -> str:
    """Read dump_path from config.local.toml if present."""
    if not CONFIG_PATH.is_file():
        return ""
    for line in CONFIG_PATH.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("dump_path"):
            _, _, rhs = stripped.partition("=")
            return rhs.strip().strip('"').strip("'")
    return ""


def write_dump_path(dump_path: str) -> None:
    """Save dump_path to config.local.toml, preserving update_version."""
    normalized = dump_path.replace("\\", "/")
    update_version = "327680"
    if CONFIG_PATH.is_file():
        for line in CONFIG_PATH.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if stripped.startswith("update_version"):
                _, _, rhs = stripped.partition("=")
                val = rhs.strip().strip('"').strip("'")
                if val:
                    update_version = val
    content = (
        "[game]\n"
        f'dump_path = "{normalized}"\n'
        f'update_version = "{update_version}"\n'
    )
    CONFIG_PATH.write_text(content, encoding="utf-8")


class MTFLauncherApp:
    """Main Tkinter application for MTF-Recomp."""

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("MTF-Recomp Launcher (Mario Tennis Fever)")
        self.root.geometry("760x560")
        self.root.minsize(680, 480)
        self.root.configure(bg="#181a1f")

        self.log_queue: queue.Queue[str] = queue.Queue()
        self.busy = False

        self.dump_path_var = tk.StringVar(value=read_dump_path())
        self.dlaa_enabled_var = tk.BooleanVar(value=True)
        self.hdr_enabled_var = tk.BooleanVar(value=False)
        self.dlss_status_var = tk.StringVar(value="Verificando...")

        self._build_ui()
        self.refresh_dlss_status()
        self.root.after(100, self._drain_log_queue)

    def _build_ui(self) -> None:
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("TFrame", background="#181a1f")
        style.configure("TLabel", background="#181a1f", foreground="#e6e6e6", font=("Segoe UI", 10))
        style.configure("Header.TLabel", background="#181a1f", foreground="#ffffff", font=("Segoe UI", 14, "bold"))
        style.configure("Status.TLabel", background="#181a1f", foreground="#9ecbff", font=("Segoe UI", 9, "bold"))
        style.configure("TCheckbutton", background="#181a1f", foreground="#e6e6e6", font=("Segoe UI", 10))
        style.configure("TButton", font=("Segoe UI", 10), padding=6)
        style.configure("Accent.TButton", font=("Segoe UI", 10, "bold"), padding=8)

        main = ttk.Frame(self.root, padding=16)
        main.pack(fill=tk.BOTH, expand=True)

        ttk.Label(main, text="MTF-Recomp — Mario Tennis Fever", style="Header.TLabel").pack(anchor=tk.W, pady=(0, 12))

        # 1. Base game path
        path_frame = ttk.Frame(main)
        path_frame.pack(fill=tk.X, pady=(0, 10))
        ttk.Label(path_frame, text="Pasta do jogo base (extraído):").pack(anchor=tk.W)

        path_row = ttk.Frame(path_frame)
        path_row.pack(fill=tk.X, pady=(4, 0))
        self.path_entry = ttk.Entry(path_row, textvariable=self.dump_path_var)
        self.path_entry.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 8))
        ttk.Button(path_row, text="Selecionar...", command=self.select_game_path).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(path_row, text="Salvar", command=self.save_game_path).pack(side=tk.LEFT)

        # 2. DLAA / DLSS & HDR options
        opts_frame = ttk.Frame(main)
        opts_frame.pack(fill=tk.X, pady=(6, 10))

        dlss_row = ttk.Frame(opts_frame)
        dlss_row.pack(fill=tk.X, pady=(0, 6))
        ttk.Label(dlss_row, text="Status DLAA/DLSS:").pack(side=tk.LEFT, padx=(0, 6))
        ttk.Label(dlss_row, textvariable=self.dlss_status_var, style="Status.TLabel").pack(side=tk.LEFT, fill=tk.X, expand=True)
        ttk.Button(dlss_row, text="Verificar / Instalar DLAA/DLSS", command=self.install_or_verify_dlss).pack(side=tk.RIGHT)

        toggles_row = ttk.Frame(opts_frame)
        toggles_row.pack(fill=tk.X)
        ttk.Checkbutton(toggles_row, text="Ativar DLAA / DLSS (MTF_DLAA=1)", variable=self.dlaa_enabled_var).pack(side=tk.LEFT, padx=(0, 18))
        ttk.Checkbutton(toggles_row, text="Ativar HDR10 ACES (TOTK_HDR=1)", variable=self.hdr_enabled_var).pack(side=tk.LEFT)

        # 3. Action buttons
        actions = ttk.Frame(main)
        actions.pack(fill=tk.X, pady=(6, 10))
        self.build_btn = ttk.Button(actions, text="Validar e Fazer Build", command=self.validate_and_build)
        self.build_btn.pack(side=tk.LEFT, padx=(0, 10))
        self.launch_btn = ttk.Button(actions, text="Iniciar Jogo", style="Accent.TButton", command=self.launch_game)
        self.launch_btn.pack(side=tk.LEFT)

        # 4. Log output
        ttk.Label(main, text="Log:").pack(anchor=tk.W, pady=(4, 4))
        self.log_box = scrolledtext.ScrolledText(
            main,
            height=14,
            bg="#0f1115",
            fg="#d4d4d4",
            insertbackground="#ffffff",
            font=("Consolas", 9),
            state=tk.DISABLED,
        )
        self.log_box.pack(fill=tk.BOTH, expand=True)

        self.log(f"Repositório: {REPO_ROOT}")
        if self.dump_path_var.get():
            self.log(f"Path salvo carregado: {self.dump_path_var.get()}")

    def log(self, text: str) -> None:
        self.log_queue.put(text)

    def _drain_log_queue(self) -> None:
        while True:
            try:
                msg = self.log_queue.get_nowait()
            except queue.Empty:
                break
            self.log_box.configure(state=tk.NORMAL)
            self.log_box.insert(tk.END, msg + "\n")
            self.log_box.see(tk.END)
            self.log_box.configure(state=tk.DISABLED)
        self.root.after(100, self._drain_log_queue)

    def _set_busy(self, busy: bool) -> None:
        self.busy = busy
        state = tk.DISABLED if busy else tk.NORMAL
        self.build_btn.configure(state=state)
        self.launch_btn.configure(state=state)

    def select_game_path(self) -> None:
        initial = self.dump_path_var.get() or str(Path.home())
        chosen = filedialog.askdirectory(title="Selecione a pasta extraída do Mario Tennis Fever", initialdir=initial)
        if chosen:
            self.dump_path_var.set(chosen)
            write_dump_path(chosen)
            self.log(f"[config] Path salvo em config.local.toml: {chosen}")

    def save_game_path(self) -> None:
        path = self.dump_path_var.get().strip()
        if not path:
            messagebox.showwarning("Path vazio", "Informe ou selecione a pasta do jogo base.")
            return
        write_dump_path(path)
        self.log(f"[config] Path salvo em config.local.toml: {path}")

    def refresh_dlss_status(self) -> None:
        target = STAGE2_DIR / DLSS_DLL_NAME
        if target.is_file():
            size_mb = target.stat().st_size / (1024 * 1024)
            self.dlss_status_var.set(f"Instalado ({DLSS_DLL_NAME}, {size_mb:.1f} MB)")
        else:
            self.dlss_status_var.set(f"Não encontrado em {STAGE2_DIR}")

    def install_or_verify_dlss(self) -> None:
        target = STAGE2_DIR / DLSS_DLL_NAME
        if target.is_file():
            self.refresh_dlss_status()
            self.log(f"[dlss] OK: {target} já está instalado.")
            messagebox.showinfo("DLAA / DLSS", f"{DLSS_DLL_NAME} já está instalado em:\n{target}")
            return

        STAGE2_DIR.mkdir(parents=True, exist_ok=True)
        for src in KNOWN_DLSS_SOURCES:
            if src.is_file():
                shutil.copy2(src, target)
                self.refresh_dlss_status()
                self.log(f"[dlss] Copiado de {src} -> {target}")
                messagebox.showinfo("DLAA / DLSS", f"{DLSS_DLL_NAME} instalada com sucesso!")
                return

        chosen = filedialog.askopenfilename(
            title=f"Localizar {DLSS_DLL_NAME}",
            filetypes=[("DLL files", "*.dll"), ("All files", "*.*")],
        )
        if chosen:
            shutil.copy2(chosen, target)
            self.refresh_dlss_status()
            self.log(f"[dlss] Copiado de {chosen} -> {target}")
            messagebox.showinfo("DLAA / DLSS", f"{DLSS_DLL_NAME} instalada com sucesso!")
        else:
            self.refresh_dlss_status()
            self.log("[dlss] Instalação cancelada; DLL não encontrada.")

    def validate_and_build(self) -> None:
        if self.busy:
            return
        path = self.dump_path_var.get().strip()
        if not path or not Path(path).is_dir():
            messagebox.showerror("Erro", "Selecione uma pasta válida do jogo base antes de validar/buildar.")
            return
        write_dump_path(path)
        self._set_busy(True)
        threading.Thread(target=self._run_build_worker, daemon=True).start()

    def _python_exe(self) -> str:
        if not getattr(sys, "frozen", False):
            return sys.executable
        return shutil.which("python") or shutil.which("py") or "python"

    def _run_build_worker(self) -> None:
        py = self._python_exe()
        try:
            self.log("=== Validando content guard ===")
            rc = self._run_streamed([py, str(REPO_ROOT / "tools" / "check_content.py")])
            if rc != 0:
                self.log(f"[erro] check_content.py retornou código {rc}")
                return

            self.log("=== Compilando mtf-stage2.exe (tools/build_stage2.py) ===")
            rc = self._run_streamed([py, str(REPO_ROOT / "tools" / "build_stage2.py")])
            if rc == 0:
                self.log("[ok] Build concluída com sucesso!")
            else:
                self.log(f"[erro] build_stage2.py retornou código {rc}")
        finally:
            self.root.after(0, lambda: self._set_busy(False))
            self.root.after(0, self.refresh_dlss_status)

    def _run_streamed(self, cmd: list[str]) -> int:
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        proc = subprocess.Popen(
            cmd,
            cwd=str(REPO_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            creationflags=creationflags,
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            self.log(line.rstrip("\r\n"))
        return proc.wait()

    def launch_game(self) -> None:
        if not STAGE2_EXE.is_file():
            messagebox.showerror("Executável não encontrado", f"Compile primeiro com 'Validar e Fazer Build'.\nEsperado: {STAGE2_EXE}")
            return

        env = os.environ.copy()
        env["MTF_DLAA"] = "1" if self.dlaa_enabled_var.get() else "0"
        env["TOTK_HDR"] = "1" if self.hdr_enabled_var.get() else "0"

        self.log(f"[launch] Iniciando {STAGE2_EXE} (MTF_DLAA={env['MTF_DLAA']}, TOTK_HDR={env['TOTK_HDR']})")
        creationflags = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
        subprocess.Popen([str(STAGE2_EXE)], cwd=str(REPO_ROOT), env=env, creationflags=creationflags)


def main() -> int:
    root = tk.Tk()
    MTFLauncherApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
