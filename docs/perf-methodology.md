# MTF Recomp — Metodologia de Métricas de Performance & Memória

Este documento define o protocolo padrão para medir ganhos reais de **estabilidade**, **uso de memória/VRAM** e **performance de CPU/GPU** ao longo das etapas de otimização (especialmente **Tier 2** em [roadmap.md](roadmap.md)).

---

## 1. Regras de Comparação A/B

1. **Mesmo hardware, mesmo build de jogo (`1.1.1`) e mesma cena:**
   - **Cenário padrão:** Partida 1v1 na *Stadium Court* (ou replay determinístico via `MTF_INPUT_SCRIPT`) por pelo menos **1.800 frames (~30s)**.
2. **Dois perfis de medição por etapa:**
   - **Perfil A — Jogabilidade Real (`60 Hz`):** `MTF_FPS_LIMIT=60`, `MTF_FRAMEGEN=off` — mede *stutters*, `1% low`, `0.1% low` e picos de alocação.
   - **Perfil B — Teto de CPU/Replay (`Destravado`):** `MTF_FPS_LIMIT=unlimited`, `VK_PRESENT=immediate`, `MTF_FRAMEGEN=off` — remove o limite de 60 FPS para medir o ganho bruto de `replay_ms/frame` e FPS máximo de CPU/GPU. *(Usar apenas em cena estática/menu ou replay curto para medir custo de backend, já que a simulação acelera sem Frame Gen.)*
3. **Toggle por variável de ambiente:** Toda otimização nova deve possuir um knob (ex.: `MTF_VRAM_HEADROOM=0/1`, `MTF_HEAP_LAZY=0/1`, `MTF_REPLAY_THREADS=0/4`) para permitir comparação A/B na mesma máquina e no mesmo binário.

---

## 2. Métricas Coletadas

Extraídas diretamente de [perfmon.c](../runtime/perfmon.c), [vk_memory.c](../runtime/vk_memory.c) e `build/fork/bin/logs/mtf.log`:

| Categoria | Métrica | Fonte no Runtime / Log | Objetivo |
|---|---|---|---|
| **Fluidez** | `FPS` médio, `1% low` (`low1_fps`), `0.1% low` (`low01_fps`) | `perfmon.c` / overlay (`F1`) | Eliminar quedas e micro-travamentos |
| **CPU Vulkan** | `replay_ms/frame`, `pipe_ms/frame`, `tex_ms/frame` | `perfmon.c` (`g_win`) | Reduzir tempo de gravação de command buffers |
| **Memória (RAM/Commit)** | `memory commit` (MB) no frame 1800 / pico | `mtf.log` (`mem: census` / `fatal` / `Win32`) | Reduzir commit para caber com folga em 16 GB RAM |
| **VRAM & Pools** | Pools importados (`MiB`), mirrors ativos, recuos de budget | `mtf.log` (`mem: census at frame ...`) | Evitar OOM / thrashing em GPUs de 6–8 GB VRAM |
| **Boot & Shaders** | Tempo de `prewarm` (ms) e `boot: guest starts` (ms) | `mtf.log` (`vk_pipeline: prewarm`) | Garantir que não há regressão de inicialização |

---

## 3. Tabela de Evolução (Baseline → Etapas Tier 2)

> Preencher após cada etapa validada e registrar os logs resumidos em [devlog.md](devlog.md).

| Marco | Commit | Memory Commit (MB) | Pools `mem: census` | `replay_ms/f` | `FPS` (Perfil B) | `1% low` / `0.1% low` (Perfil A) | Observações |
|---|---|---|---|---|---|---|---|
| **Baseline (pré-Tier 2)** | `a94cb62` | ~7.575–8.024 MB *(observado em logs anteriores)* | `350 pools / 732.9 MiB` *(frame 1800)* | *a medir* | *a medir* | *a medir* | `MTF_PIPE_SYNC=1` padrão em NVIDIA |
| **Etapa 2.1 (`vk_headroom` + `HEAP_LAZY` + `SPARSE_MIRROR_COPY` + commit)** | *este commit* | `5.076 MB` *(boot 6s; medir em partida no frame 1800)* | *a medir em partida* | *a medir* | *a medir* | *a medir* | `MTF_VRAM_HEADROOM=1` (6 blocos 256 MiB pré-alocados no boot), `MTF_HEAP_LAZY=1`, `MTF_SPARSE_MIRROR_COPY=auto`, `MTF_COMMIT_HEADROOM_MIB=512` |
| **Etapa 2.2 (`REPLAY_THREADS` / `vk_spec`)** | *pendente* | — | — | — | — | — | — |
| **Etapa 2.3 (`spirv_nan_guard`)** | *pendente* | — | — | — | — | — | — |
