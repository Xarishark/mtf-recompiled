# Contributing to MTF Recomp

Thank you for your interest in contributing to **MTF Recomp**! Please follow these guidelines carefully so the repository remains clean, legal, and maintainable.

## 1. Respect Copyright & Content Boundaries (Mandatory)

This is a **public, clean-room static recompilation repository**. **No copyrighted game assets, binaries, keys, firmware, or generated game code may ever be committed or attached to issues/PRs.**

Never commit or upload:
- `.nup`, `.nsp`, `.xci`, `.nro`, `.nso`, `.romfs`, `.exefs` files
- Encryption keys (`prod.keys`, `title.keys`), Switch firmware files, or extracted game textures/models/audio
- Generated C/C++ game code (`generated/`) or compiled shader caches (`.spv`)

Before committing or opening a Pull Request:
1. Enable the repository pre-commit hook:
   ```powershell
   git config core.hooksPath .githooks
   ```
2. Run the automated content guard:
   ```powershell
   python tools/check_content.py
   ```
   Every PR is also checked automatically by GitHub Actions (`.github/workflows/content-guard.yml`).

## 2. Licensing Rules

- Project-owned code is licensed under the **MIT License** (`LICENSE`).
- Any third-party code adapted or reused must come from **MIT, BSD, ISC, zlib, or Apache-2.0** compatible projects and be documented in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) (source repo, commit hash, license, and target files).
- **Do not copy GPL-3.0 code** into this repository.

## 3. Architecture & Code Guidelines

- **No emulation shortcuts:** Do not add a CPU interpreter/JIT or low-level GPU command-stream emulator. Lift AArch64 to C, implement Horizon OS HLE, and translate NVN2 to Vulkan 1.3.
- **Keep game-specifics isolated:** Game-specific offsets, memory layouts, shader facts, and hooks belong under `profiles/0400D4201C6EA000/`, not hardcoded across generic runtime files.
- **Never hand-edit generated C:** Fix the lifter/generator in `recomp/` instead.
- **Test your changes:** Build cleanly with `python tools/build_fork.py`, run `python -m installer.gui --selftest` if touching `installer/`, and verify `python tools/check_content.py` exits with `0`.

## 4. Reporting Bugs

When opening an issue on [GitHub Issues](https://github.com/rafael-fj/mtf-recompiled/issues), please include:
1. Your GPU model, driver version, and Windows version.
2. What you were doing when the issue occurred (menu, Free Play, multiplayer, etc.).
3. Your `logs/mtf.log` (and `mtf.ini` if relevant) — **never** attach game dumps, keys, or `.nup` files.
