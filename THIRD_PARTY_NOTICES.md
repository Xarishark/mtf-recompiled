# Third-Party Notices

Code copied or adapted from other open-source projects is listed below with its source repository, revision, license, and usage in this repository.

## Code Currently in This Repository

| Component | Source & Revision | License | Used In |
| :--- | :--- | :--- | :--- |
| Content guard (`check_content.py`) | Adapted from `valentderah/totk-pc-recomp` (`tools/check_content.py`) | MIT | `tools/check_content.py` |
| NUP container reader | `a55uka/nup` @ `a4840f06c94ddeb414eea5b33c9704ffe995f11c` | MIT (`tools/vendor/nup/LICENSE`) | `tools/vendor/nup/` |
| MTF shader workgroup facts (`shader_facts.tsv`) & Switch 2 profile metadata | `seedds/nxnative` @ `b2d46b86d03f4b03afe11ea61c67cdf00d7a5345` | MIT | `profiles/0400D4201C6EA000/shader_facts.tsv`, `profile.json` |

## Planned Upstream Dependencies (Phases F1–F8)

- `peiikai/LASRecomp` (MIT) — ARM64 lifter, C generator, FP/SIMD lowering (`recomp/`)
- `valentderah/totk-pc-recomp` (MIT) — Runtime, Vulkan backend, installer, FSR/DLAA/HDR (`runtime/`, `installer/`)
- `ly05010419/MacNativeGames` & `jhaizhou-ops/d2r-mobile` (MIT) — Horizon HLE services, audio renderer, NVN front-end
- `seedds/nxnative` (MIT) — `cra.py`, Ampere shader translation tooling (`tools/xlate/`), Switch 2 HLE behaviors
- `Revivedx/SavageWindRecomp` (MIT) — Performance overlay & two-pane settings menu
