# Switch 2 Static Recompilation — Reference RAG (`MTF-Recomp`)

> **Uso obrigatório no pipeline:** Antes de implementar qualquer issue da fase `F9` em [docs/roadmap.md](roadmap.md), consulte este documento para verificar quais repositórios de recompilação Switch 2 já resolveram o problema, qual abordagem técnica foi usada e qual é a compatibilidade de licença.

## Regra de Licença (ver `AGENTS.md` e `THIRD_PARTY_NOTICES.md`)

- `MTF-Recomp` é **MIT**.
- **Código copiável/adaptável (MIT / compatível):** `Lucas-blip-png/dkb-pc-recomp` (base `totk-pc-recomp`, MIT), `adriadam10/mkw-recomp` (MIT). Sempre registrar em `THIRD_PARTY_NOTICES.md` com hash do commit.
- **Somente referência clean-room / ideias (GPL-3.0):** `Dittobyte/Recompia`, `Alano6678/hwaoi-recomp`, `SerenityG4K/SpaceAnimalsRecomp`, `mario-ttide/Inhale`, `Elie-0066/SPR-Recomp-Project`, `weile10000/xc2-ns2-recomp`. **Nunca copiar código GPL-3.0 diretamente.**

---

## 1. Mapa dos 8 Repositórios de Referência

| Repositório | Jogo / Base | Licença | Destaques principais verificados |
|---|---|---|---|
| [Lucas-blip-png/dkb-pc-recomp](https://github.com/Lucas-blip-png/dkb-pc-recomp) *(cópia local em `work/dkb-pc-recomp/`)* | *Donkey Kong Bananza* (`totk-pc-recomp` base, C + Vulkan) | MIT | `frame_rate.c` (`REALTIME`, `FPS_LIMIT`), `vk_framegen.c` (`2x`), `dkb_wide.c` (`ULTRAWIDE`, `HUD_POSITION`, `SCENE_RES`), `FOV`, `vk_smaa.c`, `vk_dlss.c`, `HDR_NATIVE` + `spirv_hdr_shoulder.c`, `REPLAY_THREADS` + `vk_spec.c` + `vk_exec_draw_helper.c`, `vk_headroom.c`, `flight.c`, `vk_crumbs.c`, `patches.c`, `mods_scan.h`. |
| [adriadam10/mkw-recomp](https://github.com/adriadam10/mkw-recomp) | *Mario Kart World* (C + Vulkan + translator `sm86/`) | **MIT** | `21:9`/`32:9`/`48:9` ultrawide (câmera expandida nas laterais), **Frame Generation** (30→60 ou até 240 Hz), `FPS` adaptativo (`60/30/20/15`), **Mods** com navegador GameBanana + import zip/pasta + toggle individual, **5.1 surround**, **mouse camera**, remapeamento completo, **turbo (F2)**, **skip movies**, Switch 2 controllers via USB no Windows, overlay completo + benchmark A/B determinístico (`i18n/` em 20 idiomas). |
| [Dittobyte/Recompia](https://github.com/Dittobyte/Recompia) | *Pokémon Pokopia* (Switch 2, Win/Linux/macOS) | GPL-3.0 | **Resolução livre 4K+**, **Quality presets** (`Switch 2`, `High`, `Ultra` — distância de visão e nitidez de texturas), **Handheld speed-ups** (detecção de Steam Deck / Legion Go S, `16:10` sem barras), **Upscalers** (`FSR 1`, `NVIDIA Image Scaling / NIS`, `FSR 3` e `DLSS/DLAA` em evolução), **Link Play** multiplayer (LAN/Internet relay), importação de saves (Eden/Ryujinx) com backup automático, mouse em menus + ícones dinâmicos (teclado/Xbox/PS/Switch/Steam). |
| [Alano6678/hwaoi-recomp](https://github.com/Alano6678/hwaoi-recomp) | *Hyrule Warriors: Age of Imprisonment* (`totk` base + `compiler/` NUP) | GPL-3.0 | Presets `720p/1080p/1440p/4K` confirmados em capturas de GPU; documenta explicitamente que modos não-60 FPS aceleram o gameplay sem delta-time patch; `vk_headroom` / commit headroom automático no Windows (`HEAP_LAZY` / memory accounting); `frametime_hist.h`; controles teclado/mouse + SDL. |
| [SerenityG4K/SpaceAnimalsRecomp](https://github.com/SerenityG4K/SpaceAnimalsRecomp) | *Star Fox* Switch 2 (C++ + Vulkan) | GPL-3.0 | Pré-compilação completa de shaders no primeiro boot (zero stutter), **DLSS 4 / 4.5** + **FSR 3.1 / FSR 4** (experimental), tentativas de **90/120 FPS** (ainda instáveis), redução de RAM (melhora para 16 GB), suporte melhorado a AMD Vulkan, zip automático de **Bug Report** no launcher, auto-update incremental no launcher. |
| [mario-ttide/Inhale](https://github.com/mario-ttide/Inhale) | *Kirby* Switch 2 (Win + Linux AppImage) | GPL-3.0 | **Frame Generation `Auto`** funcional (`2x` fixo pode congelar em AMD/Linux), fallback por compute para GPUs sem **mesh shaders** (GTX 10 / RX 5000), `docs/compiler-memory-accounting.md`, build multiplataforma Win/Linux. |
| [Elie-0066/SPR-Recomp-Project](https://github.com/Elie-0066/SPR-Recomp-Project) | *Splatoon Raiders* (`dkb`/`totk` base + Eglantine) | GPL-3.0 | Mesmos módulos de `dkb-pc-recomp` (`frame_rate.c`, `vk_framegen.c`, `dkb_wide.c`, `dlss_mv_spv.h`, `flight.c`, `docs/dkb-frame-rate.md`, `docs/dkb-framegen.md`, `docs/hdr-native-plan.md`), adaptado para **Eglantine** (lowering de 3.690 shaders) e `raiders-plan.md`. |
| [weile10000/xc2-ns2-recomp](https://github.com/weile10000/xc2-ns2-recomp) | *Xenoblade Chronicles 2 — Switch 2 Ed.* (`nxrecomp` + `fefw-recomp`) | GPL-3.0 | **TAAU** nativo no slot de DLSS/NDSR, **FSR 4.1.2** e **FSR 3.1.6** selecionáveis em runtime (`[dlss] upscaler = fsr3`), suporte a cutscenes **AV1 4K**, XAudio2 sink, guia detalhado de troubleshooting/invariantes/mods (`docs/MODS.md`, `docs/INVARIANTS.md`). |

---

## 2. Lookup Rápido por Feature (para uso antes de atacar cada issue)

### A. FPS Alto (> 60 FPS), Delta-Time & Frame Generation
- **Problema no MTF:** Em `MTF_FPS_LIMIT=144` + `VK_PRESENT=immediate`, o jogo atingiu 144 FPS no pacer (`0x70035611b8` em `nro0`), mas **acelerou a simulação** (passo fixo de `1/60s`).
- **Referências prioritárias:**
  1. **`dkb-pc-recomp` (MIT — copiável):** `runtime/frame_rate.c`, `recomp/patches.py` (patch page `+0..+15`), `docs/dkb-frame-rate.md` (e `MTF_SCAN_DT` já em `runtime/gfx_stubs.c`) para localizar o delta-time em memória/código e desacoplar simulação de `60 Hz`.
  2. **`dkb-pc-recomp` (MIT — copiável):** `runtime/vk_framegen.c`, `runtime/fg_pace.h`, `runtime/fg_*_spv.h` para Frame Generation `2x` sem alterar a simulação de `60 FPS` (útil especialmente se algum subsistema de física/ráquet/bola do MTF quebrar acima de 60 Hz!).
  3. **`mkw-recomp` (MIT — copiável):** Frame Generation até 240 Hz e fallback adaptativo de FPS.
  4. **`Inhale` (GPL-3.0 — ideia):** Modo `Auto` de Frame Generation (evita freezes de `2x` fixo em certas GPUs/drivers).

### B. Resoluções > 4K, Supersampling, Ultrawide & FOV
- **Referências prioritárias:**
  1. **`dkb-pc-recomp` (MIT — copiável):** `runtime/resolution.c`, `runtime/dkb_wide.c` (`ULTRAWIDE`, `HUD_POSITION=edges|centered`, `SCENE_RES`, `RENDER_SCALE` até `200%`, `FOV` `70..140`).
  2. **`mkw-recomp` (MIT — copiável):** Expansão lateral real em `21:9`, `32:9` e `48:9` + `ASPECT_PATCH`.
  3. **`Recompia` (GPL-3.0 — ideia):** Resolução livre 4K+, presets de qualidade (`Switch 2` / `High` / `Ultra` ajustando LOD/distância/filtragem) e modo `16:10` handheld.

### C. Anti-Aliasing, Upscalers & HDR Nativo
- **Referências prioritárias:**
  1. **`dkb-pc-recomp` (MIT — copiável):** `runtime/vk_smaa.c` + `smaa_*_spv.h` (`SMAA low/medium/high`), `runtime/vk_dlss.c`, `runtime/spirv_hdr_shoulder.c` + `HDR_NATIVE` (`docs/hdr-native-plan.md`).
  2. **`xc2-ns2-recomp` & `SpaceAnimalsRecomp` (GPL-3.0 — ideia):** Integração de **FSR 3.1.6 / FSR 4.1.2**, **TAAU** e **DLSS 4 / 4.5** no slot de upscaling do Switch 2.
  3. **`Recompia` (GPL-3.0 — ideia):** **NVIDIA Image Scaling (NIS)** como alternativa leve ao FSR 1.

### D. Otimizações de CPU, GPU, Memória & Shaders
- **Referências prioritárias:**
  1. **`dkb-pc-recomp` (MIT — copiável):** Replay paralelo multi-thread (`REPLAY_THREADS`, `runtime/vk_spec.c`, `runtime/vk_exec_draw_helper.c`), controle de VRAM (`runtime/vk_headroom.c`, `HEAP_LAZY`, `WB_ASYNC`, `SPARSE_MIRROR_COPY`), passes SPIR-V (`spirv_rewrite.c`, `spirv_lq_fold.cpp`, `spirv_nan_guard.c`, `spirv_screen_uv.c`).
  2. **`mkw-recomp` (MIT — copiável):** Metodologia de benchmark A/B determinístico dentro da mesma execução para validar ganhos reais de FPS/frame-time.
  3. **`hwaoi-recomp` & `SpaceAnimalsRecomp` (GPL-3.0 — ideia):** Controle de commit headroom do Windows para evitar OOM em sistemas de 16 GB RAM.

### E. Controles, Mods, Diagnóstico & UX/Launcher
- **Referências prioritárias:**
  1. **`dkb-pc-recomp` (MIT — copiável):** `runtime/flight.c` (flight recorder de spikes), `runtime/vk_crumbs.c` (`GPU_CHECKPOINTS`), `runtime/rdoc.c` (RenderDoc), `runtime/mods_scan.h` (`ROMFS_MODS`/`ROMFS_PATCHES`), `MOUSE_CAMERA`, `RUMBLE_TRIGGERS`.
  2. **`mkw-recomp` (MIT — copiável):** Integração com GameBanana + importador zip/pasta de mods com toggle, mouse camera, ícones dinâmicos por controle, turbo/skip movies.
  3. **`Recompia` & `SpaceAnimalsRecomp` (GPL-3.0 — ideia):** Importador de saves (Eden/Ryujinx) com backup automático, botão de Bug Report zipado no launcher, Link Play relay.
