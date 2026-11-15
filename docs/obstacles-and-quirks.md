# MTF Recomp — Known Obstacles & Game-Specific Quirks

This catalog tracks every *Mario Tennis Fever*-specific technical obstacle using a strict 4-part format: **Symptom**, **Root Cause**, **Solution**, and **Verification**.

---

## 1. Runtime-Unpacked Game Code (`nxboot` $\rightarrow$ `nro0`)
- **Symptom:** `exefs/main` has only ~600 KB of `.text` alongside ~13.4 MB of high-entropy `.data`; static analysis of `main` alone reveals no gameplay logic or shader tables.
- **Root Cause:** `main` is a boot stub (`nxboot`) that spawns 6 worker threads to unscramble/unpack the real 24.7 MB `NRO0` (`nro0`) out of `.data`, registers `romfs/.nrr/nrr.nrr` (NRR v1), and calls `nn::ro::LoadModule`.
- **Solution (F2):** Implement `MTF_NRO_CAPTURE` in `runtime/ro.c` to capture the unpacked `nro0` during a 2-second stage-1 boot of `main` + `sdk`, verify its SHA-256 (`1ec347bc9f4664b467fc5f9b3a9336fe91e22e119fc7fb60f8cf44f0d30ce975`) against `nrr.nrr`, and lift/compile `nro0` as a first-class module.
- **Verification:** `nro0` SHA-256 matches `nrr.nrr` and `nn::ro::LoadModule` binds to the compiled native `nro0` functions.

## 2. `CentralHeap::Init` Abort at Boot (`mem_alias` Size)
- **Symptom:** `CentralHeap::Init` aborts at `sdk+0x13f698` immediately during `nninitStartup`.
- **Root Cause:** `nninitStartup` requests 16 GB of address space from `StandardAllocator` while SDK 20.5.17 retains a 1 GB node area; a 16 GB `mem_alias` region is too small.
- **Solution (F2):** Reserve `mem_alias = 0x500000000` (20 GB) and `mem_reserve = 0x650000000` (25.25 GB) in `runtime/memory.c` (per `profiles/0400D4201C6EA000/profile.json`).
- **Verification:** Game completes `nninitStartup` and reaches `nnMain`.

## 3. Boot-Stub Thread Stack-Slot Reuse Race
- **Symptom:** Intermittent crash (~1 in 12 boots) right after `ro::Initialize` / service connection.
- **Root Cause:** `nxboot` passes thread arguments in stack slots of the starter thread and reuses those slots shortly after connecting to a service, before the spawned thread has read them.
- **Solution (F2):** Implement thread-settle synchronization in `runtime/kernel.c` (`svcStartThread`) so the starter waits briefly for the child thread to enter its prologue.
- **Verification:** 20+ consecutive boots pass without stack-argument corruption.

## 4. `nn::fontll` 50 KB Work Buffer Crash (`nro0+0xaef8a8`)
- **Symptom:** `ScalableFontEngine::LoadFont` returns `201`, `SetFont` returns `301`, and the game crashes on a null pointer at `nro0+0xaef8a8` looking up `"?"`.
- **Root Cause:** MTF gives its second font engine only 50 KB (`0xc800`) of work memory; full hinted system/fallback fonts (e.g., Arial Unicode) require 56–60 KB.
- **Solution (F2):** Serve stripped *unhinted* fallback fonts (24–32 KB footprint) via `runtime/ov_font.c` or user-supplied Switch firmware fonts.
- **Verification:** `LoadFont` returns `0` on both font engines.

## 5. `CRA.` Single-File Scrambled RomFS (`rom:/42`)
- **Symptom:** RomFS contains a single 8.17 GB file named `42` (`CRA.` magic) with no embedded directory tree.
- **Root Cause:** Camelot's `nxe` engine stores the 8,671-entry file table, checksums, and suffix list inside `nro0`'s `.rodata`, scrambling 16-byte blocks with `crc32x`/`crc32cx` keys derived from `"UseUrSk1lls4G00d"` and `"Nintendo/CAMELOT"`.
- **Solution (F4):** Port `nxnative`'s `cra.py` to locate the 3 tables in `nro0` and extract `BNSH` shaders for offline SPIR-V translation, while `runtime/romfs.c` serves `rom:/42` directly to the game's own reader.
- **Verification:** All 35,045 shaders extract cleanly from `rom:/42` + `nro0`.

## 6. VP9 1440p@60fps Opening Movie Black Frames & Choppy Audio (`nn::codec`)
- **Symptom:** Title movie (368 MB VP9 2560x1440@60fps + Vorbis) shows black/corrupt frames or 60–80 ms audio gaps every 1–2 seconds.
- **Root Cause:** (a) MTF's player (`nro0+0x3b65fc`) sends one packet per submission-ready signal and ignores refusal; (b) it shares one source clock for video and audio, pausing audio whenever VP9 decoding falls behind.
- **Solution (F3):** In `runtime/ov_codec.c`, track queued + held + decoding packets together (`CV_HOLD=12`) for event signaling, decode on a dedicated host thread, and configure `libvpx` with 4 threads and `VP9D_SET_ROW_MT=1`.
- **Verification:** Title movie plays smoothly at 60 FPS with 0 audio underruns/gaps.

## 7. Blue Wash Over 3D Scenes (Passthrough Geometry Shader)
- **Symptom:** Every 3D scene with atmosphere has a solid blue tint.
- **Root Cause:** The `8x8x64` 3D atmosphere LUT is populated once per stage using a Maxwell/Ampere passthrough geometry shader (header bit 24) that writes `gl_Layer`; if the translator assumes native passthrough support without emitting primitives/layer, the LUT fails to populate.
- **Solution (F4):** Configure the shader translator (`tools/xlate/`) to lower passthrough geometry shaders portably (emitting input primitives and `gl_Layer`), or map to `VK_NV_geometry_shader_passthrough` where supported.
- **Verification:** Stadium Court atmosphere renders with natural sky/lighting colors.

## 8. Null Vertex Buffers Bound Past Stream 15 & `nvnWindowSetCrop`
- **Symptom:** Excess validation warnings / crashes on streams `16..31`, and black bands on the right/bottom of screenshots.
- **Root Cause:** MTF clears 32 vertex streams (`16..31` with null ranges) before every model, and renders at `2048x1152` inside a `2560x1440` window texture cropped via `nvnWindowSetCrop`.
- **Solution (F5):** Ignore vertex stream binds with `first + count > 16` cleanly in `nvn_api_cmd.c`, and honor `nvnWindowSetCrop` in the Vulkan swapchain blit.
- **Verification:** 0 Vulkan validation errors during gameplay; clean full-frame presentation.
