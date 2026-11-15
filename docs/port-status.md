# MTF Recomp — Port Status

**Last updated:** 2026-11-15  
**Target game:** *Mario Tennis Fever* (Switch 2 / `Ounce`, Title ID `0400D4201C6EA000`, Update `1.1.1` / `v327680`)  
**Current phase:** `F0` (Complete) $\rightarrow$ Ready for `F1` (Toolchain & Lifter)

## Module Status

| Module | File Size | Disk MD5 | Lifted | Compiled | Linked | Verified |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `main` (`nxboot`) | 14,069,153 B | `115e98ed471509dd30785b35cf8a0d58` | No | No | No | Validated (`NSO0`) |
| `sdk` (20.5.17) | 7,326,594 B | `17fd7a3ffd8eb8af7500310841d4cf54` | No | No | No | Validated (`NSO0`) |
| `nro0` (runtime-unpacked) | ~24.7 MB | SHA-256 `1ec347bc9f4664b467fc5f9b3a9336fe91e22e119fc7fb60f8cf44f0d30ce975` | No | No | No | Registered in `nrr.nrr` (NRR v1) |

## Subsystem Status

| Subsystem | Status | Notes |
| :--- | :--- | :--- |
| Dump Validation & Content Guard | **Ready** | `tools/check_dump.py` & `tools/check_content.py` passing |
| ARM64 Lifter (`recomp/`) | Not started | Planned for F1 |
| Horizon Kernel & `ro::LoadModule` | Not started | Planned for F2 |
| NVN2 Front-End & Vulkan Backend | Not started | Planned for F3–F5 |
| Shader Pipeline (`CRA.` $\rightarrow$ SPIR-V) | Profile ready | `shader_facts.tsv` imported; `cra.py` planned for F4 |
| Audio (`audren` REV15) & Video (`ov_codec`) | Not started | Planned for F3 |
