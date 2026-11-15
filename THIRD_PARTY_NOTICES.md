# Third-Party Notices

Code copied or adapted from other open-source projects is listed below with its source repository, revision, license, and usage in this repository.

## Code Currently in This Repository

| Component | Source & Revision | License | Used In |
| :--- | :--- | :--- | :--- |
| `Shader` / `hostmem.py` / `build_game.py` patterns | `valentderah/totk-pc-recomp` @ `eda417fd1df15a17eae995b17a41880771a95f2f` | MIT | `tools/check_content.py`, `tools/hostmem.py`, `recomp/` (`gen.py`, `layout.py`, `nso.py`, `module.py`, `flags.py`, `regs.py`, `jumptables.py`, `simd_c.py`, `patches.py`), `runtime/` (`cpu.h`, `simd.h`, `simd_fp.h`, `fma.c`), `tests/diff/` |
| ARMv8.3 PAC / `LDAPR*` lifter additions | `Lucas-blip-png/dkb-pc-recomp` (MIT lifter delta over `totk-pc-recomp`) | MIT | `recomp/lifter.py` |
| ARM64 lifter, C generator, FP/SIMD lowering | `peiikai/LASRecomp` @ `da35b83a9611d046a9a681aae668d7085954d37f` (via `totk-pc-recomp`) | MIT | `recomp/`, `runtime/{cpu.h,simd.h,simd_fp.h}`, `tests/diff/` |
| NUP container reader | `a55uka/nup` @ `a4840f06c94ddeb414eea5b33c9704ffe995f11c` | MIT (`tools/vendor/nup/LICENSE`) | `tools/vendor/nup/` |
| MTF shader workgroup facts (`shader_facts.tsv`) & Switch 2 profile metadata | `seedds/nxnative` @ `b2d46b86d03f4b03afe11ea61c67cdf00d7a5345` | MIT | `profiles/0400D4201C6EA000/shader_facts.tsv`, `profile.json` |

## Planned Upstream Dependencies (Phases F2–F8)

- `valentderah/totk-pc-recomp` (MIT) — Runtime, Vulkan backend, installer, FSR/DLAA/HDR (`runtime/`, `installer/`)
- `ly05010419/MacNativeGames` & `jhaizhou-ops/d2r-mobile` (MIT) — Horizon HLE services, audio renderer, NVN front-end
- `seedds/nxnative` (MIT) — `cra.py`, Ampere shader translation tooling (`tools/xlate/`), Switch 2 HLE behaviors
- `Revivedx/SavageWindRecomp` (MIT) — Performance overlay & two-pane settings menu
