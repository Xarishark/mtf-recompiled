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

## 3. Boot-Stub Thread Stack-Slot Reuse & Async Preemption Race
- **Symptom:** Intermittent `0xc0000005` crash (`addr=0xffffffffffffffff`) inside `main`'s 6-thread `nro0` unpacker (`f_7000001d80`, `exe+0x6dc4`) or `f_7000001400` (`exe+0x54a4`) when `TOTK_ASYNC_PREEMPT=1` is active.
- **Root Cause:** Asynchronous virtual-core preemption (`kernel.c`) interrupts `main`'s 6 tight MT19937 unpacker threads on Switch 2 cores `0..5` (additionally, `kernel.c` `CreateThread` initially rejected `core > 2` with `0xfa01` until updated to `core >= NCORE`).
- **Solution (F2):** Update `kernel.c` `CreateThread` (`svc #0x08`) to accept `0 <= core < NCORE` (`NCORE=6`, mask `0x3f`); run Stage-2 boot with `TOTK_ASYNC_PREEMPT=0` while investigating safe preemption points for `main`'s unpacker threads.
- **Verification:** `MTF_GPU=null TOTK_ASYNC_PREEMPT=0 mtf-stage2.exe --boot` deterministically unpacks `nro0`, runs `nro0 DT_INIT`, and reaches `nvnDeviceGetProcAddress`.

## 3a. `nn::fs::SetAllocator` Calling `fsp-srv` & `nro0` `R_AARCH64_TLSDESC` Relocations
- **Symptom:** (a) `nn::fs::SetAllocator` aborts on unimplemented `fsp-srv` via `nn::fs::detail::IsEnabledAccessLog`; (b) `loader.c` aborts on `nro0 relocation 0 has type 1031`.
- **Root Cause:** (a) SDK 20.5.17's `SetAllocator` queries `IsEnabledAccessLog` over `fsp-srv`; (b) `nro0` uses 10 `R_AARCH64_TLSDESC` (`1031`) relocations (`sym=0`, offsets `0x0..0x50`).
- **Solution (F2):** Override `nn::fs::SetAllocator` / `IsEnabledAccessLog` in `runtime/fs_host.c`, and add `tlsdesc_resolve` (`0x1000 + addend` inside each thread's `0x2000`-byte `TLS_SIZE` block) in `runtime/loader.c`.
- **Verification:** `nro0` loads cleanly with 82,193 relocations and 0 unresolved strong imports.

## 4. `nn::fontll` 50 KB Work Buffer Crash (`nro0+0xaef8a8`)
- **Symptom:** `ScalableFontEngine::LoadFont` returns `201`, `SetFont` returns `301`, and the game crashes on a null pointer at `nro0+0xaef8a8` looking up `"?"`.
- **Root Cause:** MTF gives its second font engine only 50 KB (`0xc800`) of work memory; full hinted system/fallback fonts (e.g., Arial Unicode) require 56–60 KB.
- **Solution (F2):** Serve stripped *unhinted* fallback fonts (24–32 KB footprint) via `runtime/ov_font.c` or user-supplied Switch firmware fonts.
- **Verification:** `LoadFont` returns `0` on both font engines.

## 5. `CRA.` Single-File Scrambled RomFS (`rom:/42`)
- **Symptom:** RomFS contains a single 8.17 GB file named `42` (`CRA.` magic) with no embedded directory tree.
- **Root Cause:** Camelot's `nxe` engine stores the 8,671-entry file table, checksums, and suffix list inside `nro0`'s `.rodata`, scrambling 16-byte blocks with `crc32x`/`crc32cx` keys derived from `"UseUrSk1lls4G00d"`, `"Nintendo/CAMELOT"`, and the constant offset `0xa756e6f0` (`_X10_XOR = 0x02052636 ^ 0xA756E6F0`).
- **Solution (F4):** Implemented `tools/shaders/cra.py` + `tools/shaders/cra_fast.c` and `tools/shaders/extract.py` to decrypt `rom:/42` and unpack `MMMM`/`NARC`/`SARC`/`BNSH` shaders for offline SPIR-V translation, while `runtime/romfs.c` serves `rom:/42` directly to the game's own reader.
- **Verification:** `cra.py --verify` passed with 0 errors, and `extract.py` extracted all 35,045 unique shaders (`50,287` `vs`/`ps` pairs) from `rom:/42` + `nro0`.

## 5a. SPIR-V `GenerateCall` `InvalidCastException` / `KeyNotFoundException` on `sm86` Shaders
- **Symptom:** 7/200 shaders in the initial `translate.py --limit 200` batch failed in `Ryujinx.Graphics.Shader.CodeGen.Spirv.Instructions.GenerateCall` with `InvalidCastException` (e.g., `pfx.narc`) or `KeyNotFoundException` (e.g., `sdf_font_compute.narc`).
- **Root Cause:** `AstOptimizer` / `Simplification` can pass non-`LocalVariable` operands (constants, `AstOperation` expressions, or unregistered locals) as arguments to `Instruction.Call`, whereas SPIR-V `OpFunctionCall` requires pointer operands (`Function`-storage-class `OpVariable`).
- **Solution (F4):** Added `CodeGenContext.TryGetLocalPointer` and updated `Instructions.GenerateCall` to allocate a temporary `Function`-storage-class `OpVariable` (`context.AddLocalVariable`) and store `context.Get(argType, operand)` into it whenever an argument is not a registered `LocalVariable`.
- **Verification:** `python tools/shaders/translate.py` translated all **35,045 / 35,045** shaders in `53.6s` with **0 failures**.

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

### Q8: `nn::hid::GetNpadState` (singular) vs `GetNpadStates` (plural) in MTF `nro0`
- **Symptom:** `sdk+0x64d420` aborted with `Result 0x58cca` even though `GetNpadStates` was overridden.
- **Root cause:** MTF's `nro0` imports `nn::hid::GetNpadState` (singular, 2 args) in addition to `GetNpadStates` (plural, 3 args), plus `SetNpadHandheldActivationMode`, `GetNpadControllerColor`, and `SetGyroscopeZeroDriftMode`.
- **Fix:** Extracted `fill_npad_state()` in `runtime/ov_input.c` and registered `HOST_OVERRIDE`s for all singular `GetNpadState` overloads and the remaining `nn::hid` symbols.
- **Verification:** `mtf-stage2.exe --boot` initializes all HID controllers, six-axis sensors, and vibration devices without hitting `sm: service "hid"`.

### Q9: `nn::oe` & `nn::friends` system events + missing shared fonts
- **Symptom:** After overriding `nn::oe::Initialize` and `nn::friends::Initialize`, `GetDefaultDisplayResolutionChangeEvent`, `GetNotificationMessageEvent`, and `GetFriendInvitationNotificationEvent` returned uninitialized `SystemEvent` pointers or hit `friend:u`, and `nn::pl::GetSharedFontSize(0)` aborted when Switch firmware `.bfttf` files were not provided.
- **Root cause:** MTF polls `nn::oe::GetNotificationMessageEvent` via `TimedWaitSystemEvent` and initializes `nn::friends` and `nn::fontll` during boot.
- **Fix:** Added `nn::friends` offline overrides and `nn::oe` event/resolution overrides in `runtime/ov_system.c` using `kern_bind_system_event`, plus a `C:/Windows/Fonts/arial.ttf` fallback when `.bfttf` shared fonts are absent.
- **Verification:** `mtf-stage2.exe --boot` passes `nn::friends`, `nn::oe`, and `nn::fontll` initialization cleanly.

### Q10: NVN2 `BindVertexBuffers` slots `16..31`
- **Symptom:** `nvnCommandBufferBindVertexBuffers` logs `nvn: BindVertexBuffers first 16..31 n 1` during rendering.
- **Root cause:** Switch 2 NVN2 exposes 32 vertex buffer binding slots (`0..31`), while `vk_pipeline` / `nvn_api_cmd.c` caps vertex buffer bindings at `0..15`.
- **Fix:** Silently ignore `first >= 16 && n >= 0 && first + n <= 32` in `runtime/nvn_api_cmd.c` (`bind_vertex_buffers`).
- **Verification:** Confirmed zero `BindVertexBuffers` log spam during Vulkan boot.

### Q11: `nvgpucomp64.dll+0x1909f2` Crash on `OpFunctionCall` Pointer Type Mismatch & Mid-Function `OpVariable`
- **Symptom:** `vkCreateComputePipelines` crashed inside `nvgpucomp64.dll+0x1909f2` (`addr=0x10`) on compute shader `ec0cf68e68e5f636` (and `68c74a61e9b34c61`), even though `vkCreateShaderModule` succeeded.
- **Root cause:** (a) `Spv.Generator.Module.AddLocalVariable` appended `OpVariable`s to the current block when called mid-function by `GenerateCall`; (b) `Instructions.GenerateCall` passed `LocalVariable` pointers directly even when `local.VarType != function.GetArgumentType(i)` (e.g., passing `float*` to an `int*` parameter, affecting 433 shaders).
- **Fix:** Updated `Module.AddLocalVariable` to insert at the end of the function's entry-block `OpVariable` list, and updated `Instructions.GenerateCall` to materialize a `Function`-scope `tempVar` of `argType` with bitcast copy-in (`i < InArguments.Length`) / copy-out (`i >= InArguments.Length`) whenever `local.VarType != argType`. Re-translated all 35,045 shaders.
- **Verification:** `ec0cf68e68e5f636` and all subsequent compute/graphics pipelines compile cleanly on RTX 5070 Ti.

### Q12: Unused `OpDecorate Binding` (`cb0`) Omitted from `.res.json` Reflection
- **Symptom:** `vk_pipeline: graphics pipeline not built: the vertex shader uses binding 1, which its reflection and layout lack` on `c674dc35b7a82578` and `2da4ab693163e08e`.
- **Root cause:** `Ryujinx.Graphics.Shader` can declare an unused `cb0` (`OpVariable` + `OpDecorate Binding 1`) without adding it to `CBuffers`, causing `vk_pipeline.c` (`module_binding_missing`) to reject the pipeline.
- **Fix:** In `runtime/gfx_shader.c` (`nvn2_offline_get`), scan the loaded SPIR-V for `OpDecorate ... Binding >= 1` and append any missing binding to `s->res[GFX_RES_UBO]`.
- **Verification:** Graphics pipelines for `c674dc35b7a82578` and `2da4ab693163e08e` build cleanly with 0 skipped draws.

### Q13: Black screen and no audio after boot (`StartVp9Decoder` ready-event race)
- **Symptom:** `mtf-stage2.exe --boot` opened the Vulkan window and ran the main loop, but the screen stayed black with no audio output.
- **Root cause:** `nro0` uses `main`'s `movie::VideoDecoder` (`nn::codec::Vp9Decoder`). `ov_vp9_ready_event` and `ov_vp9_acquire` signalled the submission-ready `SystemEvent` before `StartVp9Decoder` ran, while `StartVp9Decoder` was bound to `ov_ok` (no-op). `MovieEventHandler` (`nro0+0xd16460`) woke up early, called `ClearSystemEvent`, and posted an input-ready message to `MovieVideoHandler` (`nro0+0xd19c00`), which dropped it because `Start` had not yet set its active flag — leaving the decoder stalled on `RequestSyncPointEvent(0xc0de0000)`.
- **Fix:** Bound `_ZN2nn5codec15StartVp9DecoderEPNS0_14Vp9DecoderTypeE` in `runtime/ov_codec.c` to `ov_vp9_start`, which signals the submission-ready `SystemEvent` via `kern_signal_system_event(event_for(c, key))`.
- **Verification:** `mtf-stage2.exe --boot` decodes 2560x1440 VP9 video (`pass_0004_t0_2560x1440_43_rgba8srgb.png` at frame 120) and outputs non-zero audio (`first nonzero output after 182 frames`, 0 underruns).

### Q14: Crash in `nn::account::LoadProfileImage` (`0xc0000005` at `sdk+0x184f20`)
- **Symptom:** After advancing past the initial boot/movie or interacting, `mtf-stage2.exe` crashed with `0xc0000005` at offset `0x71b59b` (`f_7000f84f20`, `nn::account::LoadProfileImage`).
- **Root cause:** `nn::account::LoadProfileImage` was not overridden in `runtime/ov_system.c`, so it executed original `sdk` code calling `f_700159a2b0` (unimplemented `acc:u` IPC) which returned `NULL`, causing a null-vtable dereference.
- **Fix:** Added `ov_load_profile_image` in `runtime/ov_system.c` returning a valid 64x64 baseline JPEG (`693` bytes) for the local user UID.
- **Verification:** Rebuilt `mtf-stage2.exe` (`1.7s`), ran `TOTK_TRACE=perf,audio` cleanly at `60.0 fps` with `0` audio underruns/overruns.

### Q15: `FATAL: nvn: CallCommands with a null handle` on Title/Menu Transition
- **Symptom:** Pressing `E` (`B_A`) on the title/menu screen crashed with `FATAL: nvn: CallCommands with a null handle`.
- **Root cause:** `runtime/nvn_api_cmd.c` (`inline_recording`) aborted via `rt_fatal` when the game passed a `0` command handle to `nvnCommandBufferCallCommands`.
- **Fix:** Updated `inline_recording` in `runtime/nvn_api_cmd.c` to log once (`nvn: CallCommands: ignoring null handle`) and return early when `!handle`.
- **Verification:** Rebuilt `mtf-stage2.exe` (`0.7s`); confirmed in `work/last_launch.log` that `CallCommands` with a null handle is now logged and survived cleanly.

### Q16: `FATAL: NVN nvnCommandBufferSetViewportSwizzles is not implemented` on Menu Transition
- **Symptom:** After surviving `CallCommands` with a null handle upon pressing `E` (`B_A`), `mtf-stage2.exe` aborted with `FATAL: NVN nvnCommandBufferSetViewportSwizzles is not implemented`.
- **Root cause:** `nvn_api_core.c` reported `NVN_DEVICE_INFO_SUPPORTS_VIEWPORT_SWIZZLE = 1` (`devinfo[0x43]`), and `nvnCommandBufferSetViewportSwizzles` was still an unimplemented stub in `nvn_unimpl.inc`.
- **Fix:** Implemented `NVN_API(nvnCommandBufferSetViewportSwizzles)` in `runtime/nvn_api_cmd.c` (validates identity swizzles `(POSITIVE_X, POSITIVE_Y, POSITIVE_Z, POSITIVE_W)` and logs once if any non-identity swizzle is requested). Also wired `TOTK_DUMP=key` (`TOTK_DUMP_DIR=work/f9_dump`) into `installer/mtf_setup.py` `on_launch()` so `F9` frame dumps work out of the box.
- **Verification:** Rebuilt `mtf-stage2.exe` (`0.8s`), verified `60.0 fps` (`120 frames in 2.000 s`) and `python tools/check_content.py` (`0 violations`).

### Q17: Indexed `NVN_DRAW_PRIMITIVE_QUADS` Missing Half of Every Quad (6x4 Black Triangle Grid)
- **Symptom:** The post-title 3D tennis court (`pass 0010`, `24` tiled deferred-lighting draws) and other quad-based draws rendered only one triangle per quad, leaving a `6x4` grid of dark/black triangles across the screen.
- **Root cause:** `runtime/vk_exec_draw.c` expanded `NVN_DRAW_PRIMITIVE_QUADS` to `{0, 1, 2, 0, 2, 3}` in `op_draw` (`DrawArrays`), but `op_draw_idx` (`DrawElements`) left `NVN_DRAW_PRIMITIVE_QUADS` unexpanded while `vk_pipeline_topology` mapped it to `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`. With `d->count == 4`, Vulkan drew only the first 3 indices (1 triangle) and dropped the 4th vertex.
- **Fix:** Added `NVN_DRAW_PRIMITIVE_QUADS` index expansion (`{i0, i1, i2, i0, i2, i3}` for `q = count / 4` quads, supporting `UNSIGNED_BYTE`, `UNSIGNED_SHORT`, and `UNSIGNED_INT` host-readable index buffers) in `op_draw_idx` (`runtime/vk_exec_draw.c`).
- **Verification:** Rebuilt `mtf-stage2.exe` (`1.4s`) and dumped frame `600` (`work/dump_post_fixed/pass_0010_t0_1706x960_122_r11g11b10f_tm.png`): all 24 deferred-lighting tiles render completely with zero black triangles.

### Q18: VP9 Intro Movie Running at 20 FPS with Stuttering Audio (`ov_vp9_submit` Submission-Ready Event)
- **Symptom:** Although the render loop presented at 60 FPS, the `2560x1440` VP9 intro movie decoded only 1 video frame every ~50 ms (3 VSyncs = ~20 FPS, reaching `pts 1.70 s` in ~6 s) and audio was mostly silence (`53,493 / 239,520` nonzero callback frames over 5 s).
- **Root Cause:** In `runtime/ov_codec.c`, `ov_vp9_submit` decoded into the oldest `S_WAIT` buffer without re-signalling the `GetVp9DecoderSubmissionReadyEvent` when additional `S_WAIT` buffers were still available (`oldest_wait(d) != NULL`). The player cleared the event after submitting and therefore slept until `ReleaseVp9DecoderOutputBuffer` + `AcquireVp9DecoderOutputBuffer` ran ~50 ms later. Additionally, signalling when `oldest_wait(d) == NULL` caused the demuxer to fill the 8-entry queue and drop packets on `R_NOT_READY`, corrupting VP9 reference frames.
- **Fix:** Updated `ov_vp9_submit` in `runtime/ov_codec.c` to signal `submission-ready` when `oldest_wait(d) != NULL` and clear it (`clear_event`) when no `S_WAIT` buffer remains, and bumped `libvpx` decoder threads to `4` for `>= 1920` width (`vp9_init`).
- **Verification:** Rebuilt `mtf-stage2.exe` (`0.5s`) and ran `TOTK_TRACE=perf,codec,audio` for 8 s: zero `not ready` or VP9 decode errors, `60.0 fps` (`120 frames in 2.000 s`), `0` audio underruns/overruns/skipped, `182,982 / 239,520` nonzero audio frames, and movie PTS reached `5.07 s` smoothly.
