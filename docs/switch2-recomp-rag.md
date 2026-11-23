# Switch 2 Static Recompilation — Reference RAG (`MTF-Recomp`)

> **Mandatory pipeline usage:** Before implementing any `F9` feature in [docs/roadmap.md](roadmap.md), check this document to see which Switch 2 recompilation repositories have already solved the problem, which technical approach was used, and its license compatibility.

## License Rule (see `AGENTS.md` and `THIRD_PARTY_NOTICES.md`)

- `MTF-Recomp` is **MIT**.
- **Copyable/adaptable code (MIT / compatible):** `Lucas-blip-png/dkb-pc-recomp` (`totk-pc-recomp` base, MIT), `adriadam10/mkw-recomp` (MIT). Always record in `THIRD_PARTY_NOTICES.md` with commit hash.
- **Clean-room reference / ideas only (GPL-3.0):** `Dittobyte/Recompia`, `Alano6678/hwaoi-recomp`, `SerenityG4K/SpaceAnimalsRecomp`, `mario-ttide/Inhale`, `Elie-0066/SPR-Recomp-Project`, `weile10000/xc2-ns2-recomp`. **Never copy GPL-3.0 code directly.**

---

## 1. Reference Repository Map (8 Projects)

| Repository | Game / Base | License | Key Verified Highlights |
|---|---|---|---|
| [Lucas-blip-png/dkb-pc-recomp](https://github.com/Lucas-blip-png/dkb-pc-recomp) | *Donkey Kong Bananza* (`totk-pc-recomp` base, C + Vulkan) | MIT | `frame_rate.c` (`REALTIME`, `FPS_LIMIT`), `vk_framegen.c` (`2x`), `dkb_wide.c` (`ULTRAWIDE`, `HUD_POSITION`, `SCENE_RES`), `FOV`, `vk_smaa.c`, `vk_dlss.c`, `HDR_NATIVE` + `spirv_hdr_shoulder.c`, `REPLAY_THREADS` + `vk_spec.c` + `vk_exec_draw_helper.c`, `vk_headroom.c`, `flight.c`, `vk_crumbs.c`, `patches.c`, `mods_scan.h`. |
| [adriadam10/mkw-recomp](https://github.com/adriadam10/mkw-recomp) | *Mario Kart World* (C + Vulkan + `sm86/` translator) | **MIT** | `21:9`/`32:9`/`48:9` ultrawide (expanded side camera), **Frame Generation** (30→60 or up to 240 Hz), adaptive `FPS` (`60/30/20/15`), **Mods** with GameBanana browser + zip/folder import + individual toggle, **5.1 surround**, **mouse camera**, full remapping, **turbo (F2)**, **skip movies**, Switch 2 controllers via USB on Windows, full overlay + deterministic A/B benchmark (`i18n/` in 20 languages). |
| [Dittobyte/Recompia](https://github.com/Dittobyte/Recompia) | *Pokémon Pokopia* (Switch 2, Win/Linux/macOS) | GPL-3.0 | **Free 4K+ resolution**, **Quality presets** (`Switch 2`, `High`, `Ultra` — view distance and texture sharpness), **Handheld speed-ups** (Steam Deck / Legion Go S detection, `16:10` without bars), **Upscalers** (`FSR 1`, `NVIDIA Image Scaling / NIS`, `FSR 3`, and evolving `DLSS/DLAA`), **Link Play** multiplayer (LAN/Internet relay), save importer (Eden/Ryujinx) with auto-backup, mouse in menus + dynamic icons (keyboard/Xbox/PS/Switch/Steam). |
| [Alano6678/hwaoi-recomp](https://github.com/Alano6678/hwaoi-recomp) | *Hyrule Warriors: Age of Imprisonment* (`totk` base + `compiler/` NUP) | GPL-3.0 | `720p/1080p/1440p/4K` presets confirmed in GPU captures; explicitly documents that non-60 FPS modes speed up gameplay without a delta-time patch; `vk_headroom` / automatic Windows commit headroom (`HEAP_LAZY` / memory accounting); `frametime_hist.h`; keyboard/mouse + SDL controls. |
| [SerenityG4K/SpaceAnimalsRecomp](https://github.com/SerenityG4K/SpaceAnimalsRecomp) | *Star Fox* Switch 2 (C++ + Vulkan) | GPL-3.0 | Full shader pre-compilation on first boot (zero stutter), **DLSS 4 / 4.5** + **FSR 3.1 / FSR 4** (experimental), **90/120 FPS** attempts (still unstable), RAM reduction (16 GB improvement), improved AMD Vulkan support, automatic **Bug Report** zip in launcher, incremental auto-update in launcher. |
| [mario-ttide/Inhale](https://github.com/mario-ttide/Inhale) | *Kirby* Switch 2 (Win + Linux AppImage) | GPL-3.0 | Working **Frame Generation `Auto`** (fixed `2x` can freeze on AMD/Linux), compute fallback for GPUs without **mesh shaders** (GTX 10 / RX 5000), `docs/compiler-memory-accounting.md`, cross-platform Win/Linux build. |
| [Elie-0066/SPR-Recomp-Project](https://github.com/Elie-0066/SPR-Recomp-Project) | *Splatoon Raiders* (`dkb`/`totk` base + Eglantine) | GPL-3.0 | Same modules as `dkb-pc-recomp` (`frame_rate.c`, `vk_framegen.c`, `dkb_wide.c`, `dlss_mv_spv.h`, `flight.c`, `docs/dkb-frame-rate.md`, `docs/dkb-framegen.md`, `docs/hdr-native-plan.md`), adapted for **Eglantine** (3,690-shader lowering) and `raiders-plan.md`. |
| [weile10000/xc2-ns2-recomp](https://github.com/weile10000/xc2-ns2-recomp) | *Xenoblade Chronicles 2 — Switch 2 Ed.* (`nxrecomp` + `fefw-recomp`) | GPL-3.0 | Native **TAAU** in the DLSS/NDSR slot, **FSR 4.1.2** and **FSR 3.1.6** selectable at runtime (`[dlss] upscaler = fsr3`), **AV1 4K** cutscene support, XAudio2 sink, detailed troubleshooting/invariants/mods guide (`docs/MODS.md`, `docs/INVARIANTS.md`). |

---

## 2. Quick Feature Lookup (Check Before Tackling Each Issue)

### A. High FPS (> 60 FPS), Delta-Time & Frame Generation
- **MTF issue:** With `MTF_FPS_LIMIT=144` + `VK_PRESENT=immediate`, the game reached 144 FPS in the pacer (`0x70035611b8` in `nro0`), but **accelerated simulation** (`1/60s` fixed step).
- **Priority references:**
  1. **`dkb-pc-recomp` (MIT — copyable):** `runtime/frame_rate.c`, `recomp/patches.py` (patch page `+0..+15`), `docs/dkb-frame-rate.md` (and `MTF_SCAN_DT` already in `runtime/gfx_stubs.c`) to locate delta-time in memory/code and decouple simulation from `60 Hz`.
  2. **`dkb-pc-recomp` (MIT — copyable):** `runtime/vk_framegen.c`, `runtime/fg_pace.h`, `runtime/fg_*_spv.h` for `2x` Frame Generation without altering the `60 FPS` simulation (especially useful if any physics/racket/ball subsystem in MTF breaks above 60 Hz).
  3. **`mkw-recomp` (MIT — copyable):** Frame Generation up to 240 Hz and adaptive FPS fallback.
  4. **`Inhale` (GPL-3.0 — idea):** `Auto` Frame Generation mode (avoids fixed-`2x` freezes on certain GPUs/drivers).

### B. >4K Resolutions, Supersampling, Ultrawide & FOV
- **Priority references:**
  1. **`dkb-pc-recomp` (MIT — copyable):** `runtime/resolution.c`, `runtime/dkb_wide.c` (`ULTRAWIDE`, `HUD_POSITION=edges|centered`, `SCENE_RES`, `RENDER_SCALE` up to `200%`, `FOV` `70..140`).
  2. **`mkw-recomp` (MIT — copyable):** True side expansion in `21:9`, `32:9`, and `48:9` + `ASPECT_PATCH`.
  3. **`Recompia` (GPL-3.0 — idea):** Free 4K+ resolution, quality presets (`Switch 2` / `High` / `Ultra` adjusting LOD/distance/filtering), and `16:10` handheld mode.

### C. Anti-Aliasing, Upscalers & Native HDR
- **Priority references:**
  1. **`dkb-pc-recomp` (MIT — copyable):** `runtime/vk_smaa.c` + `smaa_*_spv.h` (`SMAA low/medium/high`), `runtime/vk_dlss.c`, `runtime/spirv_hdr_shoulder.c` + `HDR_NATIVE` (`docs/hdr-native-plan.md`).
  2. **`xc2-ns2-recomp` & `SpaceAnimalsRecomp` (GPL-3.0 — idea):** **FSR 3.1.6 / FSR 4.1.2**, **TAAU**, and **DLSS 4 / 4.5** integration in the Switch 2 upscaling slot.
  3. **`Recompia` (GPL-3.0 — idea):** **NVIDIA Image Scaling (NIS)** as a lightweight alternative to FSR 1.

### D. CPU, GPU, Memory & Shader Optimizations
- **Priority references:**
  1. **`dkb-pc-recomp` (MIT — copyable):** Multi-threaded parallel replay (`REPLAY_THREADS`, `runtime/vk_spec.c`, `runtime/vk_exec_draw_helper.c`), VRAM management (`runtime/vk_headroom.c`, `HEAP_LAZY`, `WB_ASYNC`, `SPARSE_MIRROR_COPY`), SPIR-V passes (`spirv_rewrite.c`, `spirv_lq_fold.cpp`, `spirv_nan_guard.c`, `spirv_screen_uv.c`).
  2. **`mkw-recomp` (MIT — copyable):** Deterministic in-run A/B benchmark methodology to validate real FPS/frame-time gains.
  3. **`hwaoi-recomp` & `SpaceAnimalsRecomp` (GPL-3.0 — idea):** Windows commit headroom management to avoid OOM on 16 GB RAM systems.

### E. Controls, Mods, Diagnostics & UX/Launcher
- **Priority references:**
  1. **`dkb-pc-recomp` (MIT — copyable):** `runtime/flight.c` (spike flight recorder), `runtime/vk_crumbs.c` (`GPU_CHECKPOINTS`), `runtime/rdoc.c` (RenderDoc), `runtime/mods_scan.h` (`ROMFS_MODS`/`ROMFS_PATCHES`), `MOUSE_CAMERA`, `RUMBLE_TRIGGERS`.
  2. **`mkw-recomp` (MIT — copyable):** GameBanana integration + zip/folder mod importer with toggle, mouse camera, dynamic per-controller icons, turbo/skip movies.
  3. **`Recompia` & `SpaceAnimalsRecomp` (GPL-3.0 — idea):** Save importer (Eden/Ryujinx) with auto-backup, zipped Bug Report button in launcher, Link Play relay.
