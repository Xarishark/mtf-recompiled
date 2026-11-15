# MTF Recomp

Static recompilation of *Mario Tennis Fever* (Nintendo Switch 2, Update `1.1.1` / `v327680`, Title ID `0400D4201C6EA000`) into a native Windows `x86-64` application with a Vulkan 1.3 renderer.

> [!IMPORTANT]
> **No game files are included in this repository.**
> You must provide your own legally obtained copy of *Mario Tennis Fever* (Update `1.1.1`). The recompilation and shader translation happen locally on your PC, and the resulting executable reads your game files from an external directory.

## Current Status

**Phase F0 (Foundation & Dump Validation) — Complete.**
See [docs/roadmap.md](docs/roadmap.md) for the full phase plan (`F0`–`F8`), [docs/port-status.md](docs/port-status.md) for subsystem status, and [docs/devlog.md](docs/devlog.md) for progress logs.

## Quick Start (Current Phase)

1. Configure the pre-commit content guard:
   ```powershell
   git config core.hooksPath .githooks
   ```
2. Copy `config.example.toml` to `config.local.toml` and set `[game].dump_path` to your extracted *Mario Tennis Fever + Update 1.1.1* folder.
3. Validate your dump (read-only):
   ```powershell
   python tools/check_dump.py
   ```
4. Verify no game content is staged/tracked:
   ```powershell
   python tools/check_content.py
   ```

## Documentation

- [docs/roadmap.md](docs/roadmap.md) — Step-by-step implementation roadmap (`F0`–`F8`)
- [docs/port-status.md](docs/port-status.md) — Subsystem status and verified targets
- [docs/devlog.md](docs/devlog.md) — Development and reverse-engineering log
- [docs/obstacles-and-quirks.md](docs/obstacles-and-quirks.md) — Known MTF-specific technical obstacles and solutions
- [docs/github-setup.md](docs/github-setup.md) — Recommended GitHub repository settings for `rafael-fj/mtf-recompiled`

## License & Notices

Project-owned code is licensed under the [MIT License](LICENSE). Third-party components and attributions are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

MTF Recomp is an independent, non-commercial fan project, not affiliated with or endorsed by Nintendo or Camelot.
