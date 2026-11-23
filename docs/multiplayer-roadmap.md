# Roadmap & Backlog — Online Multiplayer (`MTF-Recomp`)

Goal: Enable online multiplayer matches across separate PCs **without depending on Nintendo servers**, using a dedicated **Multiplayer** tab in the `F1` menu where **Host = Player 1** and **Guests = Player 2..4** play together in any local game mode (Free Play, 2P/4P, etc.), without relying on native *Local Play* (`nn::ldn`).

---

## Technical Decision: Why Host Screen Stream + Remote Guest Inputs?

- **Why not run two separate game instances exchanging only inputs?** In multi-threaded recompiled games with shader compilation, tiny timing/FPS differences between two PCs cause immediate physics/ball desynchronization.
- **Adopted solution (built-in Remote Play / Browser Streaming):**
  - The game runs on the **Host** PC (Player 1).
  - **Guests** (Player 2..4) open a link in Chrome/Edge (no separate install required), receive real-time **VP8 video + Opus audio**, and send controller/keyboard inputs (`input_poll_player`) back to the Host via WebRTC UDP DataChannel or WebSocket.
  - **Advantage:** Works immediately across **all local modes** (Free Play, 2P, 4P) with **zero desync**.

---

## Phase Overview

1. **Phase 0 — Multiplayer Controls & Split-Screen** *(Done)*
2. **Phase 1 & 2 — Browser Streaming Host (`F1 -> Multiplayer`) with VP8 + Opus + WebRTC UDP & Cloudflare Tunnel** *(Done)*
3. **Phase 3 — Quality Presets, Auto WebSocket Fallback & Connect/Disconnect Toasts** *(Done in `v0.1.4`)*
4. **Phase 4 (Optional / Long Term) — Native *Local Play* (`nn::ldn`) / *Online Room* (`nn::npln`)**

---

## Detailed Backlog

### Phase 0 — Multiplayer Controls & Split-Screen *(Done)*
- [x] SDL3 multi-gamepad support (`INPUT_MAX_PADS = 4`) and per-player routing (`P1..P4`) in `runtime/input.c` and `runtime/ov_hid.c`.
- [x] **Multiplayer** toggle and device selector (**Keyboard** / detected controllers) in `F1 -> Controls` (`runtime/ui/menu.cpp`).
- [x] Fixed `nn::ngc::ProfanityFilter` crash in `runtime/ov_net.c`.
- [x] Fixed DLAA/DLSS on split-screen render targets (`is_full_aspect` in `runtime/vk_dlaa.c`).

### Phase 1 & 2 — Browser Streaming Host (`F1 -> Multiplayer`) *(Done)*
- [x] Embedded HTTP + WebSocket server (`runtime/stream_host.c`) + HTML5 WebCodecs (`VP8` + `Opus`) + Gamepad/Keyboard client.
- [x] Zero-config internet tunneling via `cloudflared.exe` (`trycloudflare.com`, one-click installer & privacy modal in `F1 -> Multiplayer`).
- [x] Low-latency P2P WebRTC UDP DataChannels via `libdatachannel` (`runtime/stream_webrtc.cpp`).
- [x] **Gate 1 & 2:** Remote friend joins via browser link, controls Player 2 in Free Play, and plays full matches online.

### Phase 3 — Stability, Quality Presets & UX Polish *(Done in `v0.1.4`)*
- [x] Ordered partial-reliability media DataChannel (`150 ms` lifetime), congestion delta skipping + keyframe recovery, and `25s` WebSocket keepalive.
- [x] Automatic WebSocket fallback when a peer's NAT/firewall blocks WebRTC UDP.
- [x] Live **Stream Quality** presets (`Performance` 540p/2.2 Mbps, `Balanced` 720p/4.5 Mbps, `Quality` 720p/7.5 Mbps) persisted via `STREAM_QUALITY` in `mtf.ini`.
- [x] 3-second top-of-screen toast notification to the host when remote players connect or disconnect.

### Phase 4 — Optional / Long Term: Native *Local Play* (`nn::ldn`) / *Online Room* (`nn::npln`)
- [ ] Investigate `nn::ldn` (Local Wireless) or `nn::npln` if independent per-player screens without video streaming are desired in the future.
