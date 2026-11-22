# MTF Recomp

Static recompilation of *Mario Tennis Fever* (Nintendo Switch 2, Update `1.1.1` / `v327680`, Title ID `0400D4201C6EA000`) into a native Windows `x86-64` application with a Vulkan 1.3 renderer.

> [!IMPORTANT]
> **No game files are included in this repository.**
> You must provide your own legally obtained copy of *Mario Tennis Fever* (Update `1.1.1`). The recompilation and shader translation happen locally on your PC, and the resulting executable reads your game files from an external directory.

## Current Status

**Playable on Windows x86-64 (Vulkan 1.3).** Built on the `0.1.0+6caba99` (MIT) base fork of [kayochiaradia/mtf-recomp](https://github.com/kayochiaradia/mtf-recomp) with additional PC enhancements:
- **Frame Rate & Frame Generation:** `60 FPS` native simulation + optical-flow Frame Generation (`120`, exact monitor refresh e.g. `144 Hz`, or `unlimited` up to `600 FPS`), plus unlocked engine `FPS_LIMIT` (`30`–`unlimited`) for optimization benchmarking.
- **Resolution & Upscaling:** `720p`–`8K (4320p)`, smart `auto` matching window/display size, `50%`–`200%` render scale (supersampling), AMD FSR 1, Bilinear, and NVIDIA DLAA/DLSS.
- **Controls & UI:** Full keyboard and gamepad remapping (`F1 -> Controls`), dynamic button icons (Nintendo / Xbox / PlayStation / Keyboard), HDR10, and ImGui settings menu (`F1` / `Esc` / `Minus+Plus`) in English and Brazilian Portuguese (`pt-BR`).
- **Multiplayer & Browser Remote Play (`F1 -> Multiplayer`):** Local multi-gamepad routing (`P1`–`P4`) plus built-in browser streaming (VP8 + Opus + remote controller input) so friends can join as `P2`–`P4` directly from Chrome/Edge.
- **GUI Installer (`MTF-Recomp-Setup.exe`):** Guided Windows setup (`EN` / `pt-BR`) to pick your legal `.nup` or extracted `1.1.1` dump, choose the install directory, optionally download `nvngx_dlss.dll`, and check for GitHub updates.

## Multiplayer & Remote Play (`F1 -> Multiplayer`)

| Option | Transport | When to Use | Public IP Privacy | Expected Delay |
| :--- | :--- | :--- | :--- | :--- |
| **Local Host** | HTTP + WebSocket (direct LAN / port-forward) | Same local network (LAN/Wi-Fi) or trusted peers when you want zero tunnel overhead | Exposed if shared over WAN/port-forward (privacy warning shown) | **~15–35 ms** (LAN) |
| **Cloudflare (WebSocket)** | HTTP + WebSocket via `trycloudflare.com` tunnel | Playing over the internet when strict NAT/firewalls block UDP or you want a zero-config link | **Hidden** behind `trycloudflare.com` | **~45–90 ms** (depends on region/route) |
| **WebRTC (Low Latency UDP)** | WebRTC DataChannel (UDP) + optional Cloudflare signaling | Best responsiveness for online matches over the internet; falls back to direct local signaling if `cloudflared.exe` is not installed | **Hidden** during signaling when `cloudflared.exe` is installed (direct ICE/UDP P2P) | **~25–55 ms** (direct UDP) |

> [!TIP]
> You can install `cloudflared.exe` directly from the `F1 -> Multiplayer` tab with one click (**Install Cloudflare**).

See [docs/roadmap.md](docs/roadmap.md) (`F0`–`F9`), [docs/switch2-recomp-rag.md](docs/switch2-recomp-rag.md) (Switch 2 recomp reference RAG), [docs/port-status.md](docs/port-status.md), and [docs/devlog.md](docs/devlog.md).

## Quick Start

### Option A — GUI Installer (`MTF-Recomp-Setup.exe`)
1. Download `MTF-Recomp-Setup.exe` from [GitHub Releases](https://github.com/rafael-fj/mtf-recompiled/releases) (or run `build/release/MTF-Recomp-Setup.exe` from a local checkout).
2. Select your legal *Mario Tennis Fever* (`1.1.1`) `.nup` file or extracted `exefs`/`romfs` folder and choose your destination folder.
3. Launch `mtf-recomp.exe` from the installed folder (`F1` / `Esc` opens the settings menu).

### Option B — Build from Source (`tools/build_fork.py`)

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
