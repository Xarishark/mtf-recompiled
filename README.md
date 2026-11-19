# MTF Recomp

Static recompilation of *Mario Tennis Fever* (Nintendo Switch 2, Update `1.1.1` / `v327680`, Title ID `0400D4201C6EA000`) into a native Windows `x86-64` application with a Vulkan 1.3 renderer.

> [!IMPORTANT]
> **No game files are included in this repository.**
> You must provide your own legally obtained copy of *Mario Tennis Fever* (Update `1.1.1`). The recompilation and shader translation happen locally on your PC, and the resulting executable reads your game files from an external directory.

## Current Status

**Playable on Windows x86-64 (Vulkan 1.3).** Built on the `0.1.0+6caba99` (MIT) base fork of [kayochiaradia/mtf-recomp](https://github.com/kayochiaradia/mtf-recomp) with additional PC enhancements:
- **Frame Rate & Frame Generation:** `60 FPS` native simulation + optical-flow Frame Generation (`120`, exact monitor refresh e.g. `144 Hz`, or `unlimited` up to `600 FPS`), plus unlocked engine `FPS_LIMIT` (`30`–`unlimited`) for optimization benchmarking.
- **Resolution & Upscaling:** `720p`–`8K (4320p)`, smart `auto` matching window/display size, `50%`–`200%` render scale (supersampling), AMD FSR 1, Bilinear, and NVIDIA DLAA/DLSS.
- **HDR10, Color Grading, Audio & Controls:** ImGui settings menu (`F1` / `Esc` / `Minus+Plus`) in English and Brazilian Portuguese (`pt-BR`) with clickable dropdown lists.

See [docs/roadmap.md](docs/roadmap.md) (`F0`–`F9`), [docs/switch2-recomp-rag.md](docs/switch2-recomp-rag.md) (Switch 2 recomp reference RAG), [docs/port-status.md](docs/port-status.md), and [docs/devlog.md](docs/devlog.md).

## Quick Start

1. Configure the pre-commit content guard:
   ```powershell
   git config core.hooksPath .githooks
   ```
2. Copy `config.example.toml` to `config.local.toml` and set `[game].dump_path` to your extracted *Mario Tennis Fever + Update 1.1.1* folder.
3. Build cleanly with CMake + Ninja (`build/fork/bin/mtf-recomp.exe`):
   ```powershell
   python tools/build_fork.py
   ```
4. Verify no game content is staged/tracked:
   ```powershell
   python tools/check_content.py
   ```

## Documentation

- [docs/roadmap.md](docs/roadmap.md) — Step-by-step implementation roadmap (`F0`–`F9`)
- [docs/switch2-recomp-rag.md](docs/switch2-recomp-rag.md) — Cross-project Switch 2 recompilation RAG & license guide
- [docs/port-status.md](docs/port-status.md) — Subsystem status and verified targets
- [docs/devlog.md](docs/devlog.md) — Development and reverse-engineering log
- [docs/obstacles-and-quirks.md](docs/obstacles-and-quirks.md) — Known MTF-specific technical obstacles and solutions
- [docs/github-setup.md](docs/github-setup.md) — Recommended GitHub repository settings for `rafael-fj/mtf-recompiled`

## License & Notices

Project-owned code is licensed under the [MIT License](LICENSE). Originated from the `0.1.0+6caba99` (MIT) release of [kayochiaradia/mtf-recomp](https://github.com/kayochiaradia/mtf-recomp) (archived/taken down upstream). Third-party components and attributions are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

MTF Recomp is an independent, non-commercial fan project, not affiliated with or endorsed by Nintendo or Camelot.
