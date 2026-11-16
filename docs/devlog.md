# MTF Recomp — Development Log

## 2026-11-15 — Phase F0: Repository Setup, Reference Forensics & Dump Validation

- **Goal:** Establish a clean, legally compliant repository (`https://github.com/rafael-fj/mtf-recompiled.git`) and verify the user's local *Mario Tennis Fever* (`0400D4201C6EA000`, v1.1.1) dump without modifying any game files.
- **Findings from reference forensics:**
  - Recovered the exact component lineage of the archived `kayochiaradia/mtf-recomp` via its `THIRD_PARTY_NOTICES.md` (`totk-pc-recomp` + `LASRecomp` + `nxnative` + `MacNativeGames` + `d2r-mobile` + `nup` + `SavageWindRecomp`, keeping the entire codebase MIT-compatible).
  - Confirmed via `seedds/nxnative` (`dm/games/0400D4201C6EA000/`) that `main` is a ~600 KB `nxboot` stub that unpacks a 24.7 MB `nro0` at runtime using 6 threads, and `romfs/42` (`CRA.`) holds 8,671 files and 35,045 shaders indexed inside `nro0`'s `.rodata`.
- **Experiment (`python tools/check_dump.py`):**
  - Initial check flagged an MD5 mismatch because `nxnative/game.tsv` records MD5s of uncompressed/ELF-stage modules (`104bce68...`, `32a9ac14...`), whereas the compressed `NSO0` files on disk in `updates/327680/exefs/` have MD5s `115e98ed471509dd30785b35cf8a0d58` (`main`, 14,069,153 B) and `17fd7a3ffd8eb8af7500310841d4cf54` (`sdk`, 7,326,594 B).
  - Parsed `updates/327680/romfs/.nrr/nrr.nrr`: confirmed Switch 2 **NRR v1** (`byte[5] == 1`), `program_id = 0400D4201C6EA000`, and 1 registered NRO SHA-256 (`1ec347bc9f4664b467fc5f9b3a9336fe91e22e119fc7fb60f8cf44f0d30ce975`).
  - Confirmed `updates/327680/romfs/42` has magic `b'CRA.'` and size `8,172,385,024` B.
- **Outcome:** `tools/check_dump.py` and `tools/check_content.py` both exit `0`. Phase F0 complete.
- **Next:** Install Windows build toolchain (`MSYS2 UCRT64 Clang` / `LLVM-MinGW`, `CMake`, `Ninja`, `.NET SDK`) and begin Phase F1 (ARM64 lifter).

## 2026-11-15 — Phase F1: Toolchain Setup, ARM64 Lifter, Differential Tests & Stage-1 Link

- **Goal:** Port the MIT ARM64 $\rightarrow$ C lifter (`recomp/`), verify instruction semantics against Unicorn, and compile + link all of `main` and `sdk` into `mtf-stage1.exe`.
- **Toolchain provisioned (under git-ignored `toolchain/`):**
  - `LLVM-MinGW 20261104-ucrt-x86_64` (`clang 23.1.5` + `lld`), `Ninja 1.13.2`, Python packages `lz4`, `zstandard`, `unicorn`.
- **Experiments & Measurements:**
  - `python recomp/gen.py --exefs <dump>/updates/327680/exefs --out generated/v327680 --modules main,sdk --jobs 8`:
    - `main`: 1,589 functions, 4 chunks, 10 MB of C, 124 jump tables, 0 all-label fallbacks, 292 `br` via dispatch, **0 unsupported instructions**.
    - `sdk`: 47,907 functions, 50 chunks, 138 MB of C, 669 jump tables, 27 all-label fallbacks, 11,922 `br` via dispatch, **0 unsupported instructions**. Total lift time: **12s**.
  - `python tests/diff/lift.py --exefs <dump>/updates/327680/exefs --modules main,sdk --jobs 8`:
    - 40,000 distinct encodings, **119,955 cases executed against Unicorn: 0 failing cases, 0 failing encodings** (77s).
  - `python tools/build_stage1.py --jobs 8`:
    - Compiled all 54 generated C chunks + tables + `runtime/fma.c` + minimal runtime stub and linked `build/v327680_stage1/mtf-stage1.exe` cleanly.
    - Executed `mtf-stage1.exe`: `MTF Stage-1 link check OK: main=1589 fns, sdk=47907 fns`.
- **Outcome:** Gate F1 passed. `python tools/check_content.py` passed with 0 violations.
- **Next:** Begin Phase F2 — port Horizon OS HLE (`memory.c`, `kernel.c`, `svc.c`, `ipc.c`, `ro.c`) to boot `main`, capture the 24.7 MB `nro0` unpacked by `nxboot`'s 6 threads, and lift/compile `nro0`.

## 2026-11-15 — Phase F2: Horizon OS HLE, `nro0` Capture, Lifting & First NVN2 Call

- **Goal:** Build Stage-2 Horizon OS HLE executable (`mtf-stage2.exe`), boot `main` (`nxboot`), capture the 24.7 MB `nro0`, lift & compile `nro0`, index all modules in `build/bindb.sqlite`, and reach the first NVN2 call.
- **Implementation:**
  - Ported MIT Horizon OS HLE runtime (`kernel.c`, `memory.c`, `svc.c`, `ipc.c`, `dispatch.c`, `loader.c`, `boot.c`, `fs_host.c`, `ov_system.c`, `ov_gfx.c`, `ov_input.c`, `gfx_null.c`, `excl.c`, `platform/*`) from `dkb-pc-recomp`.
  - Implemented clean-room `runtime/ro.c` (Switch 2 NRR v1 `+0x4c0`, `MTF_NRO_CAPTURE`) and directory-backed `runtime/romfs.c` (serving `updates/327680/romfs/` directly without duplicating the 8.17 GiB `42` archive).
  - Updated `kernel.c` `CreateThread` (`svc #0x08`) to allow Switch 2's 6 application cores (`0..NCORE-1`, `NCORE=6`), `fs_host.c` to override `nn::fs::SetAllocator` / `IsEnabledAccessLog`, and `loader.c` to load `NRO0` and resolve `R_AARCH64_TLSDESC` (`1031`).
  - Extended `recomp/nso.py`, `recomp/layout.py`, and `recomp/gen.py` to support `NRO0` (`--nro name=path`) and ported `tools/analisis/bindb.py` from `mkw-recomp`.
- **Experiments & Measurements:**
  - `MTF_GPU=null MTF_NRO_CAPTURE=build/nro build/v327680_stage2/mtf-stage2.exe --boot`: captured `build/nro/1ec347bc9f4664b467fc5f9b3a9336fe91e22e119fc7fb60f8cf44f0d30ce975.nro` (`24,764,416` bytes, SHA-256 matches `nrr.nrr`).
  - `python recomp/gen.py ... --modules nro0 --nro nro0=build/nro/...nro`: lifted `nro0` (**60,968 functions**, 151 chunks, 336 MB C, 1,369 jump tables, 1 unsupported `ldst`, 54s).
  - `python tools/analisis/bindb.py`: indexed `main` + `sdk` + `nro0` into `build/bindb.sqlite` (`98,560` functions, `29,422` symbols, `573,750` calls, `54,969` strings, `77,561` fptrs).
  - `python tools/build_stage2.py --jobs 16`: compiled all 151 `nro0` chunks + `main` + `sdk` + HLE runtime into `mtf-stage2.exe` (68.9s) with zero unresolved strong imports.
  - `MTF_GPU=null TOTK_ASYNC_PREEMPT=0 build/v327680_stage2/mtf-stage2.exe --boot`: booted through `nn::ro::LoadModule` -> `nro0 DT_INIT` (197 entries) -> `nn::account` -> `save:/` mount -> **reached `nvnDeviceGetProcAddress` at `nro0+0xaf71e4`**.
- **Outcome:** Gate F2 passed. `python tools/check_content.py` verified.
- **Next:** Begin Phase F3 — implement NVN2 proc table (~700 entry points) and `MTF_GPU=null` fence/event pacing, `audren` REV15 audio, and `ov_codec` VP9 movie decoding.

## 2026-11-15 — Phase F3: NVN2 Null Renderer, Audio (REV15), Input & VP9 Movies
- **NVN2 front-end (`runtime/nvn_*.c`, `runtime/gfx_c0.c`, `runtime/tex_deswizzle.c`):** Ported MIT NVN2 proc table and command recorder from `dkb-pc-recomp`. Added `nvnQueueWaitSync` (`nvn_api_core.c`) and `nvnCommandBufferSetCopyImageStride` / `nvnCommandBufferGetCopyImageStride` (`nvn_api_cmd.c`).
- **Dependencies (`toolchain/deps-mingw/`):** Provisioned `mingw-w64-ucrt-x86_64` `SDL3` (3.4.20), `libiconv` (1.19.1), `libvpx` (1.17.0), `opus` (1.6.1), `zstd` (1.5.7), `vulkan-headers`, `vulkan-loader` (1.4.363), and `vk_mem_alloc.h` (v3.2.1).
- **Audio, Input, Pacing & Codec:** Ported `audren.c` (REV15), `audio_mixer.c`, `audio_effects.c`, `audio_reverb.c`, `input.c`, `input_map.c`, `input_script.c`, `input_win.c`, `motion_fusion.c`, `frame_rate.c`, `perf.c`, `ov_codec.c`, `ov_movie.c` from `dkb-pc-recomp`.
- **HLE additions for MTF (`ov_input.c`, `ov_system.c`):** Added `nn::hid::GetNpadState` (singular), `SetNpadHandheldActivationMode`, `GetNpadControllerColor`, `SetGyroscopeZeroDriftMode`, `GetSixAxisSensorState`, `GetSupportedNpadStyleSet`, `SetNpadJoyAssignmentModeSingle`, `SwapNpadAssignment`, `ShowControllerStrapGuide`; added `nn::friends` offline stubs; added `nn::oe::GetDefaultDisplayResolution`, `GetDefaultDisplayResolutionChangeEvent`, `GetNotificationMessageEvent`, `TryPopNotificationMessage`, `SendServerMaintenanceOverlayNotification`; added `C:/Windows/Fonts/arial.ttf` fallback in `ov_system.c` when Switch shared `.bfttf` fonts are absent.
- **Gate F3 verified:** `mtf-stage2.exe --boot` (`MTF_GPU=null`, `TOTK_ASYNC_PREEMPT=0`) reached `frame rate: limit 60 fps`, `present 1`, opened REV15 `audren` (`48000 Hz x240, mix buffers 30, voices 96, effects 14, sinks 2`) with SDL3 audio output, and initialized `2560x1440` `libvpx` VP9 decoder (`codec: VP9 decoder ... 2560x1440 on the host (libvpx)`).

## 2026-11-15 — Phase F4: `CRA.` Decryption, Shader Extraction & Ampere SPIR-V Translation
- **`rom:/42` (`CRA.`) decryption (`tools/shaders/cra.py`, `tools/shaders/cra_fast.c`):**
  - Solved Camelot `CRA.` table derivation from `nro0` `.rodata`: `_X10_XOR = 0x02052636 ^ 0xA756E6F0`, all 8 `x26 & 7` block-scrambling cases (`swap_in = (case & 2) == 0`, `odd_out = (case & 1) != 0`), `0x00` null-terminated name decoding, and extension table at `0x7003556d18` (direct file offsets).
  - Compiled `build/cra_fast.dll` with `clang -O3 -shared` for native decryption speed; verified `--pattern "ssg_system/*" --verify` (16/16 mod-16 entries, 0 errors) and `--pattern "shader/*" --verify` (8/8 mod-16 entries, 0 errors).
- **Shader extraction (`tools/shaders/extract.py`):**
  - Unpacked `MMMM` chunks, Zstd (`Z\0\0\0`), LZ4 (`0x94`), raw (`0x70`/`0x80`), zlib (`0x00`/`0x10`/`0x20`/`0x30`), `NARC`/`SARC`, and `BNSH`+`grsc` across 1,844 `rom:/42` entries + `nro0` in `20.8s`.
  - Extracted **35,045 unique shaders** (`6,915 vs`, `27,944 ps`, `78 cs`, `2 hs`, `2 ds`, `104 gs`) and **50,287 `vs`/`ps` PSO pairs** into `build/v327680/shaders-nvn2/`.
- **Ampere `sm86` SPIR-V translation (`tools/vendor/ryujinx/`, `tools/xlate/Program.cs`, `tools/shaders/translate.py`):**
  - Synced `seedds/nxnative` (MIT) `Ryujinx.Graphics.Shader` (`sm86`) and `dmsh_xlate/Program.cs` (`totk_xlate.exe` via `tools/build_xlate.py`).
  - Fixed `GenerateCall` (`CodeGen/Spirv/Instructions.cs` + `CodeGenContext.TryGetLocalPointer`) to materialize temporary `Function`-storage-class `OpVariable`s for non-`LocalVariable` call arguments emitted by `AstOptimizer`.
- **Gate F4 verified:** `python tools/shaders/translate.py` translated **35,045 / 35,045 shaders in 53.6s (654.7 shaders/s, 0 failed)** into `build/v327680/shaders-spv/`.

## 2026-11-15 — Phase F5: Vulkan 1.3 Backend, Offline NVN2 Shader Loader & 60 FPS Boot
- **Vulkan 1.3 + Window + ASTC + SPIR-V runtime (`runtime/vk_*.c`, `gfx_window*.c`, `tex_astc*.c`, `spirv_*.c`):** Ported from `dkb-pc-recomp` (MIT), integrated `clang++` (`-std=c++20`) + `-lvulkan-1 -ldwmapi -lc++` into `tools/build_stage2.py`, and pruned F5 stubs in `runtime/stage2_stubs.c`.
- **NVN2 shader loading (`runtime/vk_exec_draw.c`, `runtime/gfx_shader.c`):** Added NVN2 control header stage/length decoding (`c[0x56]`, `*(uint32_t*)(c + 0x44)`) and `nvn2_offline_get` to load pre-translated `<key16>.spv` + `<key16>.res.json` from `build/v327680/shaders-nvn2/spv/`.
- **Boot & SPIR-V fixes discovered on RTX 5070 Ti:**
  - `runtime/kernel.c` (`StartThread`): added brief startup yield (`plat_process_uptime_ns() < 2s`) to eliminate `nxboot` 6-thread stack-slot argument race.
  - `tools/vendor/ryujinx/src/Spv.Generator/Module.cs` (`AddLocalVariable`): placed function-scope `OpVariable`s in the entry block right after `OpLabel` instead of mid-function.
  - `tools/vendor/ryujinx/src/Ryujinx.Graphics.Shader/CodeGen/Spirv/Instructions.cs` (`GenerateCall`): materialized typed `Function`-scope `tempVar`s (with bitcast copy-in / copy-out) when passing a `LocalVariable` whose `VarType` differs from `function.GetArgumentType(i)` (fixed `nvgpucomp64.dll+0x1909f2` crash across 433 affected shaders). Re-translated all **35,045 / 35,045** shaders (`79.9s`, `0 failed`).
  - `runtime/gfx_shader.c` (`nvn2_offline_get`): backfilled unused `OpDecorate ... Binding >= 1` entries (`cb0`) into `GFX_RES_UBO` so `vk_pipeline.c` `layout_covers` builds all pipelines.
  - `runtime/nvn_api_cmd.c` (`bind_vertex_buffers`): silently ignored null clears on streams `16..31`.
- **Verification:** `mtf-stage2.exe --boot` (`MTF_GPU=vulkan`, `TOTK_ASYNC_PREEMPT=0`) initializes Vulkan 1.3 on RTX 5070 Ti, opens swapchain (`1920x1061`, mailbox, 60 FPS), compiles compute + graphics pipelines with 0 failures, runs VP9 `2560x1440` decoder, and presents `600+` frames cleanly. (Note: `VK_LAYER_KHRONOS_validation` is not installed on the test machine.)

## 2026-11-15 — Phase F6: PC Enhancements (FSR 1, SMAA, HDR10, PerfMon, PT-BR/EN ImGui Menu, `mtf.ini`)
- **Graphics & Perf (`runtime/vk_fsr.c`, `vk_smaa.c`, `vk_hdr.c`, `vk_usnap.c`, `perfmon.c`):** Ported MIT implementations + SPIR-V headers and `runtime/shaders/smaa/{AreaTex.h,SearchTex.h}` from `dkb-pc-recomp`. Skipped `vk_dlss.c` (`GPL-2.0-or-later`) to maintain strict MIT license compliance.
- **ImGui Settings Menu (`runtime/ui/`, `runtime/third_party/imgui/`, `runtime/ui_text.c`):** Ported Dear ImGui + `imgui_impl_vulkan` and the two-pane menu (`F1`/`Esc`/`Minus+Plus`). Added Brazilian Portuguese (`pt-BR`) catalog & auto-detection alongside English (`en`), branded to `MTF Recomp`, and switched config persistence (`runtime/cfg.c`) to `mtf.ini`.
- **Gate F6 verified:** `TOTK_MENU_TEST=30:5 TOTK_MENU_TEST_SET=20:TOTK_UPSCALER=fsr,TOTK_SMAA=high,TOTK_MENU_LANG=pt-BR,SAVE=1 TOTK_PERF_OVERLAY=full` wrote `mtf.ini`, reloaded all 3 keys on the next run, and rendered all 7 menu tabs (`0.176 ms` CPU/frame) + performance overlay cleanly.

## 2026-11-15 — Phases F7 & F8: Diagnostics, Installer & Clean Release Packaging
- **Diagnostics (F7):** Ported MIT `runtime/flight.c`, `runtime/flight.h`, `runtime/rdoc.c`, `runtime/vk_headroom.c`, and `tools/flight_view.py` from `dkb-pc-recomp`; built `mtf-stage2.exe` cleanly in `0.5s`.
- **Installer & Release Packaging (F8):** Implemented Tkinter/CLI local-build setup & launcher (`installer/mtf_setup.py`) and clean release packager (`tools/make_release.py`).
- **Gate F8 verified:** `python tools/make_release.py` staged a clean source/launcher tree, passed `tools/check_content.py` (`0 violations`), and generated `build/release/mtf-recomp-0.1.0-dev-src.zip` (`2.79 MiB`).

## 2026-11-15 — VP9 `StartVp9Decoder` Black-Screen/Audio Fix & Setup Launcher Polish
- **Root cause & fix (`runtime/ov_codec.c`):** `nro0` uses `main`'s `movie::VideoDecoder`, which calls `nn::codec::StartVp9Decoder`. `StartVp9Decoder` was previously mapped to `ov_ok` (no-op), while `GetVp9DecoderSubmissionReadyEvent` signalled the ready event prematurely before `Start`; `MovieEventHandler` (`nro0+0xd16460`) cleared the event and posted to `MovieVideoHandler` (`nro0+0xd19c00`) before `Start` set its active flag, dropping the message and stalling both video and audio. Replaced `ov_ok` with `ov_vp9_start` to signal the submission-ready `SystemEvent` on `StartVp9Decoder`.
- **Verification:** Rebuilt `mtf-stage2.exe` (`0.7s`), verified `TOTK_TRACE=audio` (`first nonzero output after 182 frames`, `0` underruns/overruns over `5.0 s`), and verified `TOTK_DUMP=120` (`work/dump_fixed/pass_0004_t0_2560x1440_43_rgba8srgb.png`) renders the 2560x1440 intro stadium movie frames.
- **Installer UX (`installer/mtf_setup.py`):** Disabled `Launch Game (Vulkan)` until `mtf-stage2.exe` exists and while a build is running, re-enabling it only after a successful build.

## 2026-11-15 — `nn::account::LoadProfileImage` Crash Fix, `1080p` Default & Menu Cleanup
- **Crash fix (`runtime/ov_system.c`):** Added `ov_load_profile_image` (`HOST_OVERRIDE` for `_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE`) returning a valid 64x64 baseline JPEG (`693` bytes), fixing `0xc0000005` at `sdk+0x184f20` (`mtf-stage2.exe+0x71b59b`).
- **Config & menu (`runtime/cfg.c`, `runtime/ui/menu.cpp`):** Excluded `mtf327680` from `dkb_output` (`2880x1620`) so `OUTPUT_RES` defaults to `1920x1080`, and removed the unimplemented `DLSS` choice from the upscaler menu (`FSR` / `Bilinear` only).
- **Launcher (`installer/mtf_setup.py`):** Guarded `on_launch()` against launching a second instance while `mtf-stage2.exe` is already running.
- **Verification:** Rebuilt `mtf-stage2.exe` (`1.7s`), `TOTK_TRACE=perf,audio` verified `60.0 fps` (`120 frames in 1.999 s`) and `0` audio underruns/overruns; `python tools/check_content.py` passed (`0 violations`).

## 2026-11-15 — `nvnCommandBufferSetViewportSwizzles` Fix & `F9` Frame Dump Support
- **NVN command buffer (`runtime/nvn_api_cmd.c`):** Implemented `nvnCommandBufferSetViewportSwizzles` (accepts identity swizzles and logs non-identity swizzles once) after `work/last_launch.log` captured `FATAL: NVN nvnCommandBufferSetViewportSwizzles is not implemented` following the `CallCommands` null-handle fix (`a0893e6`).
- **Diagnostics (`installer/mtf_setup.py`):** Enabled `TOTK_DUMP=key` and `TOTK_DUMP_DIR=work/f9_dump` by default in `on_launch()` so pressing `F9` in the game window writes a Vulkan frame dump.
- **Verification:** Rebuilt `mtf-stage2.exe` (`0.8s`), ran `TOTK_TRACE=perf,audio` (`60.0 fps`, `120 frames in 2.000 s`), verified `TOTK_MENU_TEST=210:2` with `mtf.ini` (`upscaler = bilinear`, `smaa = high`, `1920x1080`), and ran `python tools/check_content.py` (`0 violations`).

## 2026-11-15 — Indexed `NVN_DRAW_PRIMITIVE_QUADS` Fix (6x4 Black Triangle Grid)
- **Root cause & fix (`runtime/vk_exec_draw.c`):** Reproduced the post-title 3D scene via `TOTK_INPUT_SCRIPT=work/skip_to_menu.txt` (`TOTK_DUMP=600`). `pass 0010` (`24` deferred-lighting tiles, `prim 7 = NVN_DRAW_PRIMITIVE_QUADS`) used `op_draw_idx` (`DrawElements`), which did not expand 4-vertex quads into 6-index `TRIANGLE_LIST` pairs (`{i0, i1, i2, i0, i2, i3}`), causing Vulkan to drop the second triangle of every quad. Added quad index expansion in `op_draw_idx`.
- **Verification:** Rebuilt `mtf-stage2.exe` (`1.4s`) and dumped frame `600` (`work/dump_post_fixed/pass_0010_t0_1706x960_122_r11g11b10f_tm.png`): the entire 3D tennis court, shadows, net, racket, ball, and sky render cleanly without black triangles.

### 2026-11-15 — VP9 Intro Movie 20 FPS / Audio Stutter Fix (Issue #3)
- **Root cause:** `ov_vp9_submit` in `runtime/ov_codec.c` did not signal `GetVp9DecoderSubmissionReadyEvent` when `oldest_wait(d) != NULL`, throttling the 60 FPS (`2560x1440`) VP9 player to 1 frame per ~50 ms (every `Release` + `Acquire` cycle) and starving the audio stream (`53,493 / 239,520` nonzero frames).
- **Fix:** Updated `ov_vp9_submit` to signal `submission-ready` iff `oldest_wait(d) != NULL` (and `clear_event` otherwise so the demuxer never overflows `QUEUED = 8`), and increased `libvpx` decoder threads to `4` for `w >= 1920`.
- **Verification:** `python tools/build_stage2.py --jobs 16` (`0.5s`), 8s `TOTK_TRACE=perf,codec,audio` run -> `60.0 fps`, `0` codec errors/drops, `0` audio underruns (`182,982 / 239,520` nonzero frames), PTS `5.07 s`.

### 2026-11-15 — UI Font Text & NVN2 Compute `LocalSize` Fix (Issue #4)
- **Root cause:** (a) All 78 NVN2 compute shaders (`sdf_font_compute.narc`, `skinning.narc`, `blendshape_compute.narc`) read `LocalSize` from NVN1's `ctl + 0x748` instead of NVN2's `ctl + 0x58`, resulting in `LocalSize(1,1,1)`; (b) `cs_font_0`/`cs_font_1` access a `VK_FORMAT_R32_UINT` storage image via float `OpTypeImage` (`r->format == 0`) + `OpBitcast`, requiring a `VK_FORMAT_R32_SFLOAT` storage view.
- **Fix:** Updated `tools/shaders/translate.py`, `runtime/gfx_shader.c`, and `runtime/vk_exec_draw.c` to read NVN2 compute `LocalSize` from `ctl + 0x58`, re-translated all 78 NVN2 `cs` shaders (`0 failed`), and added `R32_UINT`/`R32_SINT` $\rightarrow$ `R32_SFLOAT` storage view reinterpretation in `get_view` (`runtime/vk_exec_draw.c`).
- **Verification:** `mtf-stage2.exe` rebuilt (`1.5s`). Frame `320` (`work/dump_title_text_fix2/pass_0020_t0_2560x1440_43_rgba8srgb.png`) renders `Press ? + ?`, `© Nintendo / CAMELOT`, and `Ver. 1.1.1`; frame `600` (`work/dump_post_chars_fix/pass_0038_t0_2560x1440_43_rgba8srgb.png`) renders `Let's go over some basic button controls before jumping into the game.` and `?/? End practice`.
