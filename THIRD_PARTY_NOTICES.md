# Third-Party Notices

Code copied or adapted from other open-source projects is listed below with its source repository, revision, license, and usage in this repository.

## Code Currently in This Repository

| Component | Source & Revision | License | Used In |
| :--- | :--- | :--- | :--- |
| `Shader` / `hostmem.py` / `build_game.py` patterns | `valentderah/totk-pc-recomp` @ `eda417fd1df15a17eae995b17a41880771a95f2f` | MIT | `tools/check_content.py`, `tools/hostmem.py`, `recomp/` (`gen.py`, `layout.py`, `nso.py`, `module.py`, `flags.py`, `regs.py`, `jumptables.py`, `simd_c.py`, `patches.py`), `runtime/` (`cpu.h`, `simd.h`, `simd_fp.h`, `fma.c`, `main.c` pattern), `tests/diff/` |
| Horizon OS HLE runtime, `cpu.h` (`excl_store*`), ARMv8.3 PAC / `LDAPR*` lifter | `Lucas-blip-png/dkb-pc-recomp` | MIT | `recomp/lifter.py`, `runtime/` (`boot.c`, `cfg.*`, `cpu.h`, `dispatch.*`, `excl.c`, `fs.h`, `fs_host.c`, `gfx*.h`, `gfx_null.c`, `host.h`, `hostfn.c`, `ipc.*`, `kernel.*`, `loader.c`, `memory.c`, `nvn*.h`, `ov*`, `patches.c`, `platform/*`, `romfs_path.h`, `sha256.*`, `svc.c`, `sysinfo.*`, `utf16.h`, `aoc_romfs.h`, `mods_scan.h`) |
| Binary SQLite indexer (`bindb.py`) | `adriadam10/mkw-recomp` | MIT | `tools/analisis/bindb.py` |
| ARM64 lifter, C generator, FP/SIMD lowering | `peiikai/LASRecomp` @ `da35b83a9611d046a9a681aae668d7085954d37f` (via `totk-pc-recomp`) | MIT | `recomp/`, `runtime/{cpu.h,simd.h,simd_fp.h}`, `tests/diff/` |
| NUP container reader | `a55uka/nup` @ `a4840f06c94ddeb414eea5b33c9704ffe995f11c` | MIT (`tools/vendor/nup/LICENSE`) | `tools/vendor/nup/` |
| MTF shader workgroup facts (`shader_facts.tsv`) & Switch 2 profile metadata | `seedds/nxnative` @ `b2d46b86d03f4b03afe11ea61c67cdf00d7a5345` | MIT | `profiles/0400D4201C6EA000/shader_facts.tsv`, `profile.json` |

## Planned Upstream Dependencies (Phases F2–F8)

- `valentderah/totk-pc-recomp` (MIT) — Runtime, Vulkan backend, installer, FSR/DLAA/HDR (`runtime/`, `installer/`)
- `ly05010419/MacNativeGames` & `jhaizhou-ops/d2r-mobile` (MIT) — Horizon HLE services, audio renderer, NVN front-end
- `seedds/nxnative` (MIT) — `cra.py`, Ampere shader translation tooling (`tools/xlate/`), Switch 2 HLE behaviors
- `Revivedx/SavageWindRecomp` (MIT) — Performance overlay & two-pane settings menu
- `dkb-pc-recomp` (MIT) — NVN2 front-end (`nvn_procs.c`, `nvn_api_*.c`, `nvn_ext.c`, `nvn_tex.*`, `tex_deswizzle.c`, `gfx_c0.*`, `nvn_mode_trace.*`), REV15 audio renderer & mixer (`audren.c`, `audio_mixer.c`, `audio_effects.*`, `audio_reverb.*`), input & motion fusion (`input.c`, `input_map.c`, `input_script.*`, `input_win.*`, `motion_fusion.c`), frame pacing & perf (`frame_rate.c`, `perf.c`, `prof.h`), and VP9 codec/movie (`ov_codec.c`, `ov_movie.c`).
- `GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator` `v3.2.1` (MIT) — `vk_mem_alloc.h` (local build dependency under git-ignored `toolchain/deps-mingw/`).
