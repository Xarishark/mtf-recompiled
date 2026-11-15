# MTF Recomp — Engineering & AI Agent Guidelines

Static recompilation of *Mario Tennis Fever* (Nintendo Switch 2 / `Ounce`, Title ID `0400D4201C6EA000`, Update `1.1.1` / `v327680`) into a native Windows `x86-64` executable with a Vulkan 1.3 renderer.

## 1. Strict Game-Content Boundary

This repository is public (`https://github.com/rafael-fj/mtf-recompiled`). **No game files, keys, firmware, extracted RomFS/ExeFS modules, generated game C, or compiled shader payloads may ever enter Git.**

- All game paths live in `config.local.toml` (git-ignored). Never read or modify the user's original game folder in place; treat it as strictly read-only.
- Everything extracted, captured, or generated (`nro0`, generated C, SPIR-V, binaries, SQLite binary DBs) goes under `build/`, `generated/`, or `work/` (all git-ignored).
- Every generated C/C++ file produced by `recomp/` must include the marker defined as `GENERATED_MARKER` in `tools/check_content.py`.
- Run `python tools/check_content.py` before any handoff, and ensure `.githooks/pre-commit` is active (`git config core.hooksPath .githooks`).

## 2. Licensing & Attribution

- Project-owned code is **MIT** (`LICENSE`).
- Only copy code from MIT, BSD, ISC, zlib, or Apache-2.0 compatible projects (`totk-pc-recomp`, `LASRecomp`, `MacNativeGames`, `d2r-mobile`, `nxnative`, `nup`, `SavageWindRecomp`).
- Do **not** copy GPL-3.0 code (e.g., `eglantine`, `eden`, `yuzu`) into this repository; ideas and clean-room behavior specifications only.
- Every reused or adapted component must be documented in `THIRD_PARTY_NOTICES.md` with its source repository, commit hash, license, and target files.

## 3. Code & Architecture Rules

- **No emulation shortcuts:** Do not embed a CPU interpreter/JIT or low-level Maxwell/Ampere command-stream emulator. Lift AArch64 to C, implement Horizon OS HLE, and translate NVN2 to Vulkan.
- **Keep game-specifics isolated:** Game-specific hashes, memory layouts, shader facts, and address hooks belong in `profiles/0400D4201C6EA000/` or clearly separated profile tables, never hardcoded ad-hoc across generic runtime files.
- **Never hand-edit generated C:** Fix the lifter/generator in `recomp/` instead.
- **Verify every milestone:** Generation, C compilation, linking, boot, NRO capture, null-GPU loop, shader translation, and gameplay are distinct milestones. Only mark a checklist item `[x]` in `docs/roadmap.md` after running and observing its verification gate.

## 4. Documentation Discipline

Every engineering session must keep these living documents up to date:
- `docs/roadmap.md` — Master phase checklist (`F0`–`F8`).
- `docs/port-status.md` — Current subsystem status and verified metrics.
- `docs/devlog.md` — Chronological log of experiments, commands run, and measurements.
- `docs/obstacles-and-quirks.md` — Symptom -> Root Cause -> Fix -> Verification entries.
