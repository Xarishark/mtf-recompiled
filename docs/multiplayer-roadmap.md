# Roadmap & Backlog — Multiplayer Online (`MTF-Recomp`)

Objetivo: Permitir partidas multiplayer entre máquinas distintas pela internet **sem depender dos servidores da Nintendo**, usando uma aba dedicada **Multiplayer** no menu `F1` (Criar Sala / Entrar via Código) onde o **Host = Player 1** e o **Convidado = Player 2..4** jogam juntos em qualquer modo local do jogo (Free Play, 2P/4P, etc.), sem depender do *Local Play* (`nn::ldn`) nativo.

---

## Decisão Técnica: Por que Stream de Tela (Host) + Inputs Remotos (Guest)?

- **Por que não rodar duas instâncias separadas trocando apenas inputs?** Em jogos recompilados com múltiplas threads e compilação de *shaders*, pequenas variações de *timing*/FPS entre dois PCs causam dessincronização imediata (*desync* de física/bola).
- **Solução adotada (Estilo Remote Play / Parsec embutido):**
  - O jogo roda no PC do **Host** (Player 1).
  - Os **Convidados** (Player 2..4) conectam via **Código de Sala** no menu `F1`, recebem o stream de vídeo/áudio da partida (já temos `libvpx` e `opus` integrados no projeto!) e enviam seus inputs (`input_poll_player`) por UDP de baixa latência para o Host.
  - **Vantagem:** Funciona imediatamente em **todos os modos locais** do jogo (Free Play, 2P, 4P) com **zero desync**.

---

## Visão Geral das Fases

1. **Fase 0 — Controles Multiplayer & Split-Screen** *(Concluído)*
2. **Fase 1 — MVP Sala P2P no Menu `F1` (Host P1 + Convidado P2 por Código)** *(Prioridade Atual)*
3. **Fase 2 — Stream de Vídeo/Áudio de Baixa Latência (`libvpx`/`opus`) + Suporte P2..P4**
4. **Fase 3 — Servidor Gateway / Relay Leve (Oculta IP do Host + fura NAT/CGNAT) + Tela *Online Room***
5. **Fase 4 (Opcional / Longo Prazo) — *Local Play* (`nn::ldn`) ou *Online Room* Nativo (`nn::npln`)**

---

## Backlog Detalhado

### Fase 0 — Controles Multiplayer & Split-Screen *(Concluído)*
- [x] Suporte multi-gamepad SDL3 (`INPUT_MAX_PADS = 4`) e roteamento por jogador (`P1..P4`) em `runtime/input.c` e `runtime/ov_hid.c`.
- [x] Toggle **Multiplayer** e seletor de dispositivo (**Teclado** / controles detectados) na aba **Controles** do menu `F1` (`runtime/ui/menu.cpp`).
- [x] Correção do crash de `nn::ngc::ProfanityFilter` em `runtime/ov_net.c`.
- [x] Correção do DLAA/DLSS em alvos *split-screen* (`is_full_aspect` em `runtime/vk_dlaa.c`) para renderizar as duas câmeras normalmente.

### Fase 1 — MVP Sala P2P no Menu `F1` (Host P1 + Convidado P2 por Código) *(Prioridade Atual)*
- [ ] Criar aba/seção **Multiplayer** no menu `F1` (`runtime/ui/menu.cpp` / nova página `runtime/ui/mp_page.cpp`):
  - **Criar Sala (Host):** gera um **Código de Sala** ofuscado (`MTF-XXXX-XXXX`, codificando `IPv4 + Porta` + checksum), tenta liberar a porta via UPnP quando disponível e ativa o servidor UDP embutido.
  - **Entrar em Sala (Convidado):** campo para colar o **Código de Sala**, conectar ao Host via UDP e assumir automaticamente o slot **Player 2** (ou próximo slot livre).
  - Status ao vivo da sala (conectado/desconectado, ping em ms, jogadores na sala).
- [ ] Implementar camada de transporte de **Input Remoto** sobre UDP (`runtime/netplay.c` / `runtime/netplay.h`):
  - Pacote compacto de input (~24 bytes/tick: `buttons`, `lx`, `ly`, `rx`, `ry`, `seq`, `ping_ts`) enviado pelo Convidado e injetado diretamente no `input_poll_player(1..3)` do Host via `ov_hid.c`.
- [ ] **Gate 1:** Convidado conecta ao Host pelo Código de Sala no `F1`, aparece como **Player 2** conectado no *Free Play* e controla o P2 em tempo real com latência medida no log/UI.

### Fase 2 — Stream de Vídeo/Áudio do Host para o Convidado (P2..P4)
- [ ] Captura leve do framebuffer apresentado pelo Vulkan (`gfx_present.c` / `vk_present.c`) no Host e codificação em tempo real usando `libvpx` (já linkado em `CMakeLists.txt`) + áudio mixado (`opus`, também já linkado).
- [ ] Modo **Cliente Leve** no Convidado: ao entrar em uma sala via Código, exibe o stream de vídeo/áudio do Host na janela do jogo em tela cheia/janela enquanto envia os controles locais para o Host.
- [ ] Suporte completo para até **4 jogadores** (Host = P1, Convidados = P2..P4).
- [ ] **Gate 2:** Partida completa de *Free Play* pela internet entre 2 PCs distintos usando apenas o Código de Sala no menu `F1`.

### Fase 3 — Servidor Gateway / Relay Leve + Tela *Online Room* no `F1`
- [ ] Desenvolver servidor Gateway leve (MIT, auto-hospedável e compatível com planos gratuitos):
  - Listagem de **Salas Públicas** e **Salas Privadas por Código** (sem nunca expor o IP residencial do Host aos convidados).
  - **Ponte UDP (Relay)** para furar NAT/CGNAT sem precisar abrir portas no roteador.
- [ ] Evoluir a aba **Multiplayer** do `F1` para o layout completo de **Online Room** (abas de filtro, lista de salas, **Select Room**, **Search by Room ID**, **Create Room** Pública/Privada e **Refresh**).
- [ ] **Gate 3:** Jogadores atrás de NAT/CGNAT encontram salas públicas ou entram por código privado com IP do Host protegido.

### Fase 4 — Opcional / Longo Prazo: *Local Play* (`nn::ldn`) / *Online Room* Nativo (`nn::npln`)
- [ ] Investigar e destravar `nn::ldn` (Local Wireless) ou `nn::npln` caso queiramos no futuro tela independente por jogador sem streaming de vídeo.
