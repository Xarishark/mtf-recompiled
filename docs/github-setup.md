# GitHub Repository Configuration Guide (`rafael-fj/mtf-recompiled`)

Practical recommendations for configuring the GitHub repository for **legal safety (DMCA / anti-leak)**, **engineering quality**, and **release workflow**.

## 1. Branch Protection Rules for `main` (Settings → Rules → Rulesets)
- **Require `Content Guard` as a mandatory Status Check:** Require `.github/workflows/content-guard.yml` to pass before any merge into `main`. This prevents accidental PRs or pushes containing NSOs, NROs, keys (`prod.keys`), generated C code, or binary files `> 1 MiB`.
- **Block Force Push** on `main` to preserve `devlog.md` history and attributions in `THIRD_PARTY_NOTICES.md`.

## 2. GitHub Actions & Repository Features
- **GitHub Issues & Labels by Phase/Subsystem:** Maintain labels aligned with the roadmap:
  - `phase:F1-lifter`, `phase:F2-os-nro`, `phase:F3-nvn-audio-codec`, `phase:F4-shaders-cra`, `phase:F5-vulkan`, `phase:F6-pc-features`, `phase:F7-diagnostics`, `phase:F8-installer`
  - `area:lifter`, `area:kernel`, `area:nvn2`, `area:vulkan`, `area:shaders`, `area:audio-video`
- **GitHub Releases (do not use Git LFS for toolchain binaries):** Releases (`MTF-Recomp-v<version>-Windows-x64.zip` and `mtf-recomp-<version>-src.zip`) are built cleanly on GitHub Actions via `.github/workflows/release.yml` — containing **zero game files** — and attached to GitHub Releases.
- **`CONTRIBUTING.md` Policy:** Explicitly state that issues or PRs containing dumps, links to ROMs, keys, or binaries compiled with game code will be closed and removed immediately.

## 3. Recommended Local Workflow
- Always keep the local pre-commit hook enabled:
  ```powershell
  git config core.hooksPath .githooks
  ```
- Create feature branches per phase or task (e.g., `feat/f1-lifter-setup`, `feat/f2-nro-capture`) and merge into `main` only after verifying its gate.
