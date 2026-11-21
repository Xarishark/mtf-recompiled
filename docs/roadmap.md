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

## F2 — Horizon OS HLE, Boot Stub (`nxboot`) & `nro0` Capture (24.7 MB) (Complete)

- [x] Implement guest memory layout (`runtime/memory.c`: 64 GiB alias, 9 GiB pool), Switch 2 `svcGetInfo`, `31.25 MHz` timer (`rt_ticks`), 6-core threads (`NCORE = 6`), mutexes, condvars, and IPC (`runtime/kernel.c`, `runtime/svc.c`, `runtime/ipc.c`).
- [x] Implement `runtime/ro.c` (NRR v1 + `MTF_NRO_CAPTURE`) to capture the 24.7 MB `nro0` unpacked by `main`'s 6 threads, verify its SHA-256 (`1ec347bc9f4664b467fc5f9b3a9336fe91e22e119fc7fb60f8cf44f0d30ce975`), index `main` + `sdk` + `nro0` in `tools/analisis/bindb.py`, and lift/compile `nro0` (60,968 fns, 151 chunks).
- [x] Implement directory-backed `rom:/` VFS mounting (`runtime/romfs.c`), `nn::fs` host storage (`runtime/fs_host.c`), `R_AARCH64_TLSDESC` relocation support (`runtime/loader.c`), and Stage-2 subsystem stubs (`runtime/stage2_stubs.c`).
- [x] **Gate F2:** `mtf-stage2.exe` boots `main` + `sdk` + `nro0` (with `TOTK_ASYNC_PREEMPT=0`), mounts `rom:/` and `save:/`, completes `nro0 DT_INIT` (197 entries), and reaches the first NVN2 call (`nvnDeviceGetProcAddress` at `nro0+0xaf71e4`).

## F3 — NVN2 Null Renderer, Audio (REV15) & VP9 Movies (`nn::codec`) (Complete)

- [x] Implement NVN2 proc table (~700 entry points, `nvnQueueWaitSync`, `nvnCommandBufferSetCopyImageStride`) with `MTF_GPU=null` fence/event pacing (`frame_rate.c`).
- [x] Port `audren.c` (REV15) + SDL3 audio output (`audio_mixer.c`, `audio_effects.c`, `audio_reverb.c`) and input (`input.c`, `input_win.c`, `ov_input.c`, `motion_fusion.c`, `GetNpadState`, `SetNpadHandheldActivationMode`, `GetNpadControllerColor`, `SetGyroscopeZeroDriftMode`).
- [x] Port async `ov_codec.c` (`libvpx` VP9 2560x1440 decoder) + `ov_movie.c`, plus `nn::friends` offline stubs, `nn::oe` display resolution/notification event overrides, and system TTF fallback for missing shared fonts.
- [x] **Gate F3:** `mtf-stage2.exe --boot` with `MTF_GPU=null` and `TOTK_ASYNC_PREEMPT=0` runs at 60 Hz (`frame rate: limit 60 fps`, `present 1`), opens REV15 `audren` + SDL3 audio (`48000 Hz x240, voices 96`), and initializes `2560x1440` `libvpx` VP9 video decoder cleanly.

## F4 — `CRA.` Extraction & Offline Shader Translation (35,045 Shaders) [Complete]

- [x] Implement `tools/shaders/cra.py` (+ `cra_fast.c`), `extract.py`, `translate.py`, and `tools/xlate/` (Ryujinx Ampere `sm86` front-end + `GenerateCall` SPIR-V fix).
- [x] Handle passthrough geometry shader lowering (`SPV_NV_geometry_shader_passthrough` / `8x8x64` atmosphere LUT) and NVN2 compute `c[0]`/`c[1]` workgroup indexing (`shader_facts.tsv`).
- [x] **Gate F4:** All 35,045 shaders from `rom:/42` and `nro0` extract and translate to SPIR-V without fatal errors (`35045/35045` in `53.6s`, `0 failed`).

## F5 — Vulkan 1.3 Backend & Playable 60 FPS

- [x] Implement Switch 2 texture GOB deswizzling (`tex_deswizzle.c`), ASTC (`tex_astc.c`, `vk_astc.c`), `nvnWindowSetCrop`, and silent ignore of null vertex streams `16..31` (`nvn_api_cmd.c`).
- [x] Implement Vulkan 1.3 renderer, recorder thread (`vk_rec.c`), offline NVN2 SPIR-V loader (`gfx_shader.c`), and SPIR-V `OpVariable` / `GenerateCall` / unused `c0` binding fixes (`Module.cs`, `Instructions.cs`, `gfx_shader.c`).
- [ ] **Gate F5:** Opening movie, menus, Adventure intro, and Free Play singles on Stadium Court render at 60 FPS with 0 Vulkan validation errors (60 FPS Vulkan boot, swapchain presentation, 600+ frames, and VP9 2560x1440 opening movie verified; awaiting interactive Stadium Court playthrough and `VK_LAYER_KHRONOS_validation` check).

## F6 — PC Features (4K, DLSS/DLAA/FSR1, HDR10, Gyro, PT-BR/EN Menu)

- [x] Port FSR 1 (`vk_fsr.c`), SMAA (`vk_smaa.c`), HDR10 (`vk_hdr.c`), uniform snapshot (`vk_usnap.c`), and `perfmon.c`.
- [x] Port ImGui two-pane settings menu (`runtime/ui/`, `runtime/third_party/imgui/`, `F1`/`Esc`/`Minus+Plus`) in English and Brazilian Portuguese (`pt-BR`) + `mtf.ini` persistence.
- [x] **Gate F6:** PC enhancement options (`TOTK_UPSCALER=fsr`, `TOTK_SMAA=high`, `TOTK_MENU_LANG=pt-BR`, `TOTK_PERF_OVERLAY=full`) toggle live, render across all 7 menu tabs (`menu-test-35-0.png`..`65-6.png`), and persist/reload via `mtf.ini`. (Optional NVIDIA NGX DLSS/DLAA skipped for MIT license compliance.)

## F7 — Diagnostics, Frame Replay & Visual Bugfixes

- [x] Port flight recorder & thread snapshots (`runtime/flight.c`, `flight.h`, `tools/flight_view.py`), RenderDoc integration (`runtime/rdoc.c`), and VRAM headroom tracker (`runtime/vk_headroom.c`).
- [ ] Investigate close-up grass blade rendering and non-NVIDIA shadow portability (ongoing visual QA).

## F8 — End-User Installer (`MTF-Setup.exe`) & Release Packaging

- [x] Implement Tkinter local-build installer & launcher ([installer/mtf_setup.py](installer/mtf_setup.py)) + clean release packager ([tools/make_release.py](tools/make_release.py)).
- [x] **Gate F8:** `python tools/make_release.py` produces a clean `2.79 MiB` release archive (`build/release/mtf-recomp-0.1.0-dev-src.zip`) with **0 content-guard violations** and zero game files.

## F9 — Fork 1:1 (`0.1.0+6caba99`) Feature & Optimization Roadmap

Base: clean 1:1 fork (`build/fork/bin/mtf-recomp.exe` via `tools/build_fork.py`).

> **Regra do Pipeline:** Antes de atacar qualquer issue abaixo, consultar [docs/switch2-recomp-rag.md](switch2-recomp-rag.md) para reaproveitar soluções comprovadas (código MIT copiável ou referência clean-room para GPL-3.0).

1. **FPS Alto (> 60 FPS) & Frame Generation**
   - [x] Expor `MTF_FPS_LIMIT` (`30|40|45|60|90|120|144|240|display|unlimited`) no `cfg.c`, menu in-game (`F1`) e pacer de `nro0` (`0x70035611b8`): medido **144 FPS** (`display`) e **~1925 FPS** (`unlimited` na abertura) — útil para benchmark de otimizações.
   - [x] **FPS > 60 em velocidade real `1x` funciona via Frame Generation (`MTF_FRAMEGEN`):** portado de `mkw-recomp` / `SkyboundTotk` (MIT, `vk_framegen.c` + `fg_*_spv.h`, modos `off|120|monitor|unlimited` com swapchain de 5 imagens em `gfx_present.c` para até `4x` = `240 FPS`). Validado em jogo pelo usuário (`60 FPS` base + `2x` = `120 FPS` fluidos a `1x`).
   - [ ] *(Opcional / futuro)* Desacoplar simulação nativa do `60 Hz` (`frame_rate.c` + delta-time patches): teste em partida com `MTF_FPS_LIMIT=144` sem Frame Gen confirmou aceleração (~2.4x), pois o jogo usa passo fixo de `1/60s`.
2. **Resolução de Renderização, Upscaling & Câmera/Ultrawide**
   - [x] **Fluxo claro de Resolução de Renderização + Upscaling + >4K/Supersampling:** `OUTPUT_RES=auto` agora acompanha a janela/tela (`1080p` em janela `1080p`, sem downscale forçado de `1440p`), `ov_resolution.c` suporta até `5K (2880p)` / `8K (4320p)` e `RENDER_SCALE` vai de `50%` até `200%` (supersampling); menu (`F1`) reorganizado na ordem Escala de renderização -> Resolução de saída -> Upscaler (`FSR 1`/`Bilinear`) -> `DLAA/DLSS`.
   - [ ] *(Opcional / futuro)* Avaliar `FSR 3.1` / `NIS` do RAG (`xc2-ns2-recomp`, `Recompia`) e implementar `ULTRAWIDE` (`21:9`/`32:9`), `HUD_POSITION` e `FOV`.
   - [ ] Implementar `ULTRAWIDE`, `HUD_POSITION` (`edges|centered`) e `ASPECT_PATCH`.
   - [ ] Implementar `FOV` (`res_fov_live()`), `WINDOW_MODE` (`windowed|borderless|fullscreen`) e `DEVICE_PROFILE`.
3. **Anti-Aliasing, Upscaling & HDR Nativo**
   - [ ] Portar `SMAA` (`vk_smaa.c` + `smaa_*_spv.h`).
   - [ ] Portar `HDR_NATIVE` + `spirv_hdr_shoulder.c`.
   - [ ] Expor `SHADOW_RES` e `SHADOW_SCALE` no menu in-game (`metas[]` em `runtime/cfg.c`).
4. **Otimizações de CPU, GPU & Memória (Vulkan Backend — Tier 2, Prioridade Atual)**
   Metodologia de medição A/B e tabela de evolução em [docs/perf-methodology.md](perf-methodology.md).
   - **Etapa 2.1 — Memória, VRAM & Redução de Stutter (PCs de 16 GB RAM / 8 GB VRAM):**
     - [x] Portar `vk_headroom.c` (`MTF_VRAM_HEADROOM`, pré-alocação de blocos VMA em background com recuo automático acima de 85% do orçamento de VRAM).
     - [x] Portar `MTF_HEAP_LAZY` (`runtime/platform/win32.c` `fault_veh` + `runtime/memory.c` `mem_touch`) e `MTF_SPARSE_MIRROR_COPY` (`runtime/vk_memory.c` + `mirror_sparse_spv.h`). *(Nota: `MTF_WB_ASYNC`/`MTF_WB_UNLOCKED` pertencem ao write-back assíncrono de `vk_texture.c`, fora desta etapa.)*
     - [x] Implementar controle de *commit headroom* no Windows (`MTF_COMMIT_HEADROOM_MIB`, padrão `512` MiB, clean-room via `GlobalMemoryStatusEx` `ullAvailPageFile` em `plat_mem_avail_commit`) para evitar OOM em 16 GB RAM.
     - [x] **Gate 2.1:** `check_content.py` limpo, build sem erros e smoke test de boot verificando ativação de `MTF_VRAM_HEADROOM`, `MTF_HEAP_LAZY`, `MTF_SPARSE_MIRROR_COPY=auto` e `MTF_COMMIT_HEADROOM_MIB` (`5076 MB` no boot inicial); medições em partida registradas em [docs/perf-methodology.md](perf-methodology.md).
   - **Etapa 2.2 — Performance de CPU no Replay Vulkan:**
     - [x] Portar replay paralelo multi-thread (`MTF_REPLAY_THREADS`, padrão `6` em >=12 CPUs / `4` em >=8 CPUs / `0` em AMD) em [runtime/vk_exec.c](runtime/vk_exec.c) + `DrawCtx` por thread em [runtime/vk_exec_draw.c](runtime/vk_exec_draw.c) + suporte de queries herdadas em [runtime/vk_exec_xfer.c](runtime/vk_exec_xfer.c) + write-back local por thread em [runtime/vk_memory.c](runtime/vk_memory.c). *(Nota: `MTF_REC_PUB` em `vk_rec.c` e `vk_pipeline_prewarm(..., cpus)` em `vk_pipeline.c` já cobriam o papel de `REC_BATCH` e `PREWARM_XLATE` no runtime.)*
     - [x] Avaliar `vk_spec.c` + `vk_exec_draw_helper.c` (`MTF_REPLAY_PAR`): mantido como opcional/futuro, pois `MTF_REPLAY_THREADS=6` já paraleliza os passes grandes (>=256 draws) sem duplicar `vk_exec_draw.c` em `vk_exec_draw_helper.c`.
     - [x] **Gate 2.2:** Build limpo, `check_content.py` sem violações e smoke test confirmando `MTF_REPLAY_THREADS=6, the default`; medições A/B (`MTF_REPLAY_THREADS=0` vs `6`) em partida registradas em [docs/perf-methodology.md](perf-methodology.md).
   - **Etapa 2.3 — Robustez de Shaders & Documentação:**
     - [x] Portar `spirv_nan_guard.c` (`MTF_NAN_GUARD` opcional para fragment shaders: `0|zero|mark`).
     - [x] Registrar créditos MIT de `dkb-pc-recomp` (`4e7f9b2`) em `THIRD_PARTY_NOTICES.md`.
5. **Diagnóstico, Mods & Pipeline de Build**
   - [ ] Portar `flight.c` (`FLIGHT`), `vk_crumbs.c`, `prof_win.c`/`perf.c`, `rdoc.c`, `mods_scan.h` (`ROMFS_MODS`/`ROMFS_PATCHES`), `MOUSE_CAMERA` e `RUMBLE_TRIGGERS`.
   - [ ] Manter `tools/build_fork.py` + `tools/mtf_launcher.py` alinhados à build limpa do fork.
