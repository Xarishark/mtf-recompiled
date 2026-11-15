# GitHub Repository Configuration Guide (`rafael-fj/mtf-recompiled`)

Recomendações práticas para configurar o repositório no GitHub visando **segurança jurídica (DMCA/anti-vazamento)**, **qualidade de engenharia** e **fluxo de releases**.

## 1. Regras de Proteção da Branch `main` (Settings $\rightarrow$ Rules $\rightarrow$ Rulesets)
- **Ativar `Content Guard` como Status Check obrigatório:** Exija que o workflow `.github/workflows/content-guard.yml` passe antes de qualquer merge em `main`. Isso impede que qualquer PR ou push acidental suba NSOs, NROs, chaves (`prod.keys`), código C gerado ou arquivos $> 1$ MiB.
- **Bloquear Force Push** na branch `main` para preservar o histórico do `devlog.md` e das atribuições em `THIRD_PARTY_NOTICES.md`.

## 2. GitHub Actions e Recursos do Repositório
- **GitHub Issues & Labels por Fase/Subsistema:** Crie labels alinhadas ao roadmap:
  - `phase:F1-lifter`, `phase:F2-os-nro`, `phase:F3-nvn-audio-codec`, `phase:F4-shaders-cra`, `phase:F5-vulkan`, `phase:F6-pc-features`, `phase:F7-diagnostics`, `phase:F8-installer`
  - `area:lifter`, `area:kernel`, `area:nvn2`, `area:vulkan`, `area:shaders`, `area:audio-video`
- **GitHub Releases (não use Git LFS para binários de toolchain):** Na Fase `F8`, os pacotes `MTF-Setup-<version>-windows.zip` (contendo apenas o instalador, `LLVM-MinGW`, `CMake`, `Ninja`, `.NET` e o runtime compilado — **sem nenhum arquivo do jogo**) devem ser anexados em **GitHub Releases**, mantendo o repositório Git leve.
- **Política de `SECURITY.md` e `CONTRIBUTING.md`:** Deixe explícito que issues ou PRs contendo dumps, links para ROMs, chaves ou binários compilados com código do jogo serão fechados e removidos imediatamente.

## 3. Fluxo Local Recomendado
- Sempre trabalhe com o hook local ativado:
  ```powershell
  git config core.hooksPath .githooks
  ```
- Crie branches por fase ou sub-tarefa (ex.: `feat/f1-lifter-setup`, `feat/f2-nro-capture`) e faça merge em `main` apenas quando o **Gate** daquela fase estiver verificado.
