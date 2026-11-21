# Roadmap & Backlog — Multiplayer Online / Link Play (`MTF-Recomp`)

Objetivo: Permitir partidas multiplayer entre máquinas distintas pela internet **sem depender dos servidores da Nintendo**, começando pelo **Local Play (`nn::ldn` + `nn::socket` UDP)** e evoluindo para um **Gateway/Relay leve** com suporte a salas públicas e privadas por código (inspirado no fluxo de *Online Room*).

---

## Visão Geral das Fases

1. **Fase 0 — Controles & Split-Screen Local** *(Concluído)*
2. **Fase 1 — Destravamento do Local Play Nativo (`nn::ldn` + `nn::socket` UDP)** *(Em andamento)*
3. **Fase 2 — MVP P2P Direto com Código de Sala Ofuscado (Sem Servidor)**
4. **Fase 3 — Servidor Gateway / Relay Leve (Oculta IP do Host + NAT Traversal) + UI *Online Room* no `F1`**
5. **Fase 4 (Opcional / Longo Prazo) — Redirecionamento da UI Nativa de *Online Room* (`nn::npln`)**

---

## Backlog Detalhado

### Fase 0 — Controles Multiplayer & Split-Screen *(Concluído)*
- [x] Suporte multi-gamepad SDL3 (`INPUT_MAX_PADS = 4`) e roteamento por jogador (`P1..P4`) em `runtime/input.c` e `runtime/ov_hid.c`.
- [x] Toggle **Multiplayer** e seletor de dispositivo (**Teclado** / controles detectados) na aba **Controles** do menu `F1` (`runtime/ui/menu.cpp`).
- [x] Correção do crash de `nn::ngc::ProfanityFilter` em `runtime/ov_net.c` ao entrar/sair do modo Local.
- [x] Correção do DLAA/DLSS em alvos *split-screen* (`is_full_aspect` em `runtime/vk_dlaa.c`) para renderizar as duas câmeras normalmente.

### Fase 1 — Destravamento do Local Play Nativo *(Prioridade Atual)*
- [x] Auditoria de símbolos de rede em `build/bindb.sqlite`: `nn::ldn` + `nn::nifm` + `nn::socket` UDP/EventFd.
- [x] Implementação inicial *clean-room* MIT de `runtime/ldn.c` / `runtime/ldn.h` (`LDN1` sobre UDP, sub-rede virtual `10.114.0.0/16`).
- [ ] Instrumentar logs detalhados de chamadas `nn::ldn`, `nn::nifm` e `nn::socket` em `runtime/ov_net.c` para diagnosticar por que o menu **Local Play** retorna à tela inicial com indicador de *loading*.
- [ ] Ajustar máquina de estados e retornos de `nn::ldn` / `nn::nifm` / `nn::account` até abrir o lobby de **Criar Sala / Procurar Sala** do Local Play dentro do jogo.
- [ ] **Gate 1:** Duas instâncias de `mtf-recomp.exe` no mesmo PC (ou mesma LAN) criam sala, descobrem via `Scan`, conectam via `Connect` e trocam pacotes UDP de partida em `nn::socket`.

### Fase 2 — MVP P2P Direto com Código de Sala (Sem Servidor)
- [ ] Implementar codificador/decodificador de **Código de Sala** (`MTF-XXXX-XXXX`) em `runtime/ldn.c` empacotando `IPv4 + Porta` com ofuscação + *checksum*.
- [ ] Tentar mapeamento automático de porta UDP (`MTF_LDN_PORT`, padrão `11452`) no roteador via UPnP/IGD no Host (com fallback manual/VPN).
- [ ] Adicionar mini-painel no menu `F1` para **Gerar Código da Sala (Host)** e **Colar Código da Sala (Cliente)**.
- [ ] **Gate 2:** Partida completa pela internet entre 2 PCs distintos usando apenas o Código de Sala P2P.

### Fase 3 — Servidor Gateway / Relay Leve + Tela *Online Room* no `F1`
- [ ] Desenvolver servidor Gateway leve (MIT, auto-hospedável e compatível com planos gratuitos):
  - Gerenciamento de **Salas Públicas** (listagem) e **Salas Privadas por Código** (sem expor o IP real do Host aos clientes).
  - **Ponte UDP (Relay):** encaminha apenas pacotes `nn::ldn` / `nn::socket` entre os membros enquanto a simulação da partida continua 100% no Player Host.
  - Modo opcional de *UDP Hole Punching* para conexões diretas quando a privacidade de IP não for exigida.
- [ ] Integrar cliente Gateway em `runtime/ldn.c`.
- [ ] Construir aba/tela **Online Room** no menu `F1` (inspirada na UI de referência: abas de filtro, **Select Room**, **Search by Room ID**, **Create Room** Pública/Privada e **Refresh**).
- [ ] **Gate 3:** Dois jogadores atrás de NAT/CGNAT se encontram pela lista pública ou por Código Privado, sem expor IP residencial e sem abrir portas no roteador, completando uma partida estável.

### Fase 4 — Opcional / Longo Prazo: UI Nativa de *Online Room* (`nn::npln`)
- [ ] Investigar e mapear endpoints gRPC/HTTPS de `nn::npln` + `nn::ssl` para redirecionar o botão nativo **Online Play** do jogo para o servidor Gateway da Fase 3.
