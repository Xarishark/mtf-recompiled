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
