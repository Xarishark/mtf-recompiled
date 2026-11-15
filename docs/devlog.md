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
