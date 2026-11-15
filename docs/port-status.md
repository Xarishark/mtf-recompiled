# MTF Recomp — Port Status

**Last updated:** 2026-11-15  
**Target game:** *Mario Tennis Fever* (Switch 2 / `Ounce`, Title ID `0400D4201C6EA000`, Update `1.1.1` / `v327680`)  
**Current phase:** `F2` (Complete) $\rightarrow$ Ready for `F3` (NVN2 Null Renderer, Audio REV15 & VP9 Movies)

## Module Status

| Module | File Size | Disk MD5 / SHA-256 | Lifted | Compiled | Linked | Verified |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `main` (`nxboot`) | 14,069,153 B | MD5 `115e98ed471509dd30785b35cf8a0d58` | **1,589 fns** (10 MB C) | **Yes** (4 chunks) | **Yes** (`mtf-stage2.exe`) | Boots & unpacks `nro0` |
| `sdk` (20.5.17) | 7,326,594 B | MD5 `17fd7a3ffd8eb8af7500310841d4cf54` | **47,907 fns** (138 MB C) | **Yes** (50 chunks) | **Yes** (`mtf-stage2.exe`) | Boots & initializes HLE |
| `nro0` (runtime-unpacked) | 24,764,416 B | SHA-256 `1ec347bc9f4664b467fc5f9b3a9336fe91e22e119fc7fb60f8cf44f0d30ce975` | **60,968 fns** (336 MB C) | **Yes** (151 chunks) | **Yes** (`mtf-stage2.exe`) | Reaches `nvnDeviceGetProcAddress` |

## Subsystem Status

| Subsystem | Status | Notes |
| :--- | :--- | :--- |
| Dump Validation & Content Guard | **Ready** | `tools/check_dump.py` & `tools/check_content.py` passing |
| ARM64 Lifter (`recomp/`) | **Ready** | 0 unsupported on `main`+`sdk`, 1 `ldst` on `nro0`; 119,955 Unicorn diff cases passed |
| Binary Index (`tools/analisis/bindb.py`) | **Ready** | `build/bindb.sqlite` indexes 98,560 fns, 29,422 syms, 573,750 calls, 54,969 strings |
| Horizon Kernel & `ro::LoadModule` | **Booting** | `mtf-stage2.exe` boots `main` + `sdk` + `nro0` to `nvnDeviceGetProcAddress` (`TOTK_ASYNC_PREEMPT=0`) |
| NVN2 Front-End & Vulkan Backend | Not started | Planned for F3–F5 |
| Shader Pipeline (`CRA.` $\rightarrow$ SPIR-V) | Profile ready | `shader_facts.tsv` imported; `cra.py` planned for F4 |
| Audio (`audren` REV15) & Video (`ov_codec`) | Not started | Planned for F3 |
