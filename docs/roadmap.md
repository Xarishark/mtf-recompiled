# MTF Recomp — Implementation Roadmap (`F0`–`F8`)

Target: *Mario Tennis Fever* (Nintendo Switch 2 / `Ounce`, Title ID `0400D4201C6EA000`, Update `1.1.1` / `v327680`).

## F0 — Foundation, Content Guard & Dump Validation (Complete)

- [x] Initialize repository structure, `.gitignore`, `.gitattributes`, `LICENSE` (MIT), `AGENTS.md`, `THIRD_PARTY_NOTICES.md`.
- [x] Implement `tools/check_content.py`, `.githooks/pre-commit`, and `.github/workflows/content-guard.yml`.
- [x] Create `config.example.toml` and local `config.local.toml` pointing read-only to the user's dump.
- [x] Port `tools/vendor/nup` and `profiles/0400D4201C6EA000/` (`profile.json`, `shader_facts.tsv`).
- [x] **Gate F0:** `python tools/check_dump.py` validates `main`, `sdk`, `main.npdm`, `nrr.nrr` (NRR v1), and `romfs/42` (`CRA.`), and `python tools/check_content.py` passes with 0 violations.

## F1 — Toolchain Setup & ARM64 Lifter (`main` + `sdk`) (Complete)

- [x] Install/configure build toolchain (`LLVM-MinGW 20261104-ucrt-x86_64` Clang 23.1.5 + `Ninja 1.13.2` in `toolchain/`, Python `lz4`/`zstandard`/`unicorn`).
- [x] Port `recomp/` (`nso.py`, `module.py`, `lifter.py`, `simd_c.py`, `jumptables.py`, `gen.py`, `layout.py`, `patches.py`) from `totk-pc-recomp` / `LASRecomp`.
- [x] Support Switch 2 / ARMv8.3 instructions (`PAC` hints/branches, `LDAPR*`, `LSE` atomics, `CRC32X`/`CRC32CX`) and `DT_RELR` relocations.
- [x] **Gate F1:** 100% of `main` (1,589 fns) and `sdk` (47,907 fns) lift to C with 0 unsupported instructions, pass Unicorn differential tests (40,000 encodings, 119,955 cases, 0 failures), compile, and link (`mtf-stage1.exe`).

## F2 — Horizon OS HLE, Boot Stub (`nxboot`) & `nro0` Capture (24.7 MB)

- [ ] Implement guest memory layout (`mem_reserve=0x650000000`, `mem_alias=0x500000000`, `mem_stack=0x10000000`, `app_pool=0x240000000`).
- [ ] Implement Switch 2 `svcGetInfo`, `31.25 MHz` timer, threads (with *thread settle* for `nxboot`'s stack-slot reuse), mutexes, condvars, and IPC.
- [ ] Implement `runtime/ro.c` (NRR v1 + `MTF_NRO_CAPTURE`) to capture the 24.7 MB `nro0` unpacked by `main`'s 6 threads, verify its SHA-256 (`1ec347bc...`), index it in `tools/analisis/bindb.py`, and lift/compile `nro0`.
- [ ] Implement `rom:/42` VFS mounting, `ov_font.c` (unhinted fonts for Camelot's 50 KB font engine), and offline stubs (`ov_net.c`, `ov_offline.c`).
- [ ] **Gate F2:** Executable boots `main` + `sdk` + `nro0`, mounts `rom:/42`, initializes fonts without error `201`, and reaches the first NVN2 call.

## F3 — NVN2 Null Renderer, Audio (REV15) & VP9 Movies (`nn::codec`)

- [ ] Implement NVN2 proc table (~700 entry points) with `MTF_GPU=null` fence/event pacing.
- [ ] Port `audren.c` (REV15) + SDL3 audio output (stereo & 5.1) and `ov_hid.c` (SDL3 gamepad + keyboard).
- [ ] Implement async `ov_codec.c` (`CV_HOLD=12` event accounting, `libvpx` with 4 threads + `VP9D_SET_ROW_MT=1` for 1440p@60fps opening movie).
- [ ] **Gate F3:** Game runs at 60 Hz under `MTF_GPU=null`, plays opening movie audio without choppiness, and navigates menus via input.

## F4 — `CRA.` Extraction & Offline Shader Translation (35,045 Shaders)

- [ ] Port `tools/shaders/cra.py`, `extract.py`, `int_inputs.py`, `translate.py`, and `tools/xlate/` (Ryujinx Ampere front-end).
- [ ] Handle passthrough geometry shader (`NV_geometry_shader_passthrough` / `8x8x64` atmosphere LUT) and NVN2 compute `c[0]` workgroup indexing.
- [ ] **Gate F4:** All 35,045 shaders from `rom:/42` and `nro0` extract and translate to SPIR-V without fatal errors.

## F5 — Vulkan 1.3 Backend & Playable 60 FPS

- [ ] Implement Switch 2 texture GOB deswizzling (`tex_deswizzle.c`), ASTC (`tex_astc.c`), `nvnWindowSetCrop` (`2048x1152` inside `2560x1440`), and silent ignore of null vertex streams `16..31`.
- [ ] Implement recorder thread, conditional rendering replay, and `pso_manifest.tsv` prewarm.
- [ ] **Gate F5:** Opening movie, menus, Adventure intro, and Free Play singles on Stadium Court render at 60 FPS with 0 Vulkan validation errors.

## F6 — PC Features (4K, DLSS/DLAA/FSR1, HDR10, Gyro, PT-BR/EN Menu)

- [ ] Port resolution scaling (up to 4K), FSR 1, NVIDIA DLAA/DLSS, and HDR10 reconstruction from HDR light buffer.
- [ ] Port ImGui two-pane settings menu (`F1`/`Esc`/`Minus+Plus`) in English and Brazilian Portuguese + `perfmon.c` + SDL3 gyro.
- [ ] **Gate F6:** All PC enhancement options toggle cleanly in-game and persist in `mtf.ini`.

## F7 — Diagnostics, Frame Replay & Visual Bugfixes

- [ ] Implement GPU command capture/replay (`MTF_CAPTURE=1`), `MTF_SCAN_DT`, and thread snapshots.
- [ ] Investigate close-up grass blade rendering and non-NVIDIA shadow portability.

## F8 — End-User Installer (`MTF-Setup.exe`) & Release Packaging

- [ ] Port Tkinter local-build installer supporting `.nup` and extracted folders + `tools/make_release.py`.
- [ ] **Gate F8:** Clean build on a fresh machine produces a playable `mtf-recomp.exe` from the user's dump with zero game files in the release archive.
