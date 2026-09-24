# Pioneer DJ-LINK (Pro DJ Link) — Draft Spec / Plan

Status: draft (2026-09-23), branch `codex/ddj400-jc1060-integration`.
Reference: https://github.com/Deep-Symmetry/dysentery and
https://djl-analysis.deepsymmetry.org/djl-analysis/ (EPL-1.0 — analysis only,
no code copied).

## Naming warning (history)

Upstream previously carried a feature called "CDJ Link" (`cdj_link_protocol`,
`cdj_link_server`, `cdj_link_client`, `remote_cache`) — removed in upstream
commits `8718c64` / `ee8db05` (2026-06/07). That was a **custom Pajoniiir-to-
Pajoniiir library-sharing protocol** (magic "CJL1", UDP 42424, HTTP 8080) —
NOT Pioneer's Pro DJ Link. New components must be named `dj_link_*` to avoid
reviving that confusion, and the old removed code is irrelevant as a starting
point.

## Why the P4 is the right endpoint

- `firmware/main-deck-p4` already parses rekordbox `export.pdb` +
  `ANLZ*.DAT/EXT` (BPM, beat grid, waveform, cues) —
  `docs/rekordbox-format-analysis.md`. The beat grid is exactly what a real
  CDJ uses to emit beat packets, and what we need to align our playback to
  incoming beats.
- P4 is the authoritative playback/audio/UI engine (AGENTS.md rule).
- Ethernet is proven on the JC1060 board (IP101 RMII, `8a11e2f` eth_bringup,
  MDC=31/MDIO=52/REF_CLK in GPIO50, OTA over ETH proven). DJ Link is a
  LAN-UDP protocol — no Wi-Fi dependency. `eth_bringup.c` has link up/down
  but no netif/IP yet: DHCP + netif attach is step 0.

## Protocol summary (from djl-analysis)

Magic header on every packet: `51 73 70 74 31 57 6d 4a 4f 4c`
("Qspt1WmJOL") + 1 type byte, port-dependent. Little-endian throughout.

UDP port 50000 — discovery / channel assignment:
- `06` keep-alive (CDJ ~2.00 s interval, mixer 1.5 s; do NOT use 1.5 s for
  device timeout of players). Byte 0x25 = "was I first device on network".
- `00`/`02`/`04` stage 1/2/final channel-number claims; `08` channel
  conflict; `0a` initial announcement.

UDP port 50001 — beat/sync:
- `28` beat packet (0x60 bytes, broadcast on every beat by playing devices
  with rekordbox-analyzed tracks; mixers emit a backup metronome). Contains
  ms-to-next-beat/bar/4th/8th, pitch (`0x00100000` = 0%), BPM x100, beat-in-bar.
- `0b` absolute position (CDJ-3000+, every 30 ms even when paused): playhead
  ms, pitch x64x100, BPM x10 — reliable position even while scratching/loops.
- `2a` sync control, `26`/`27` tempo-master handoff, `02` fader start,
  `03` channels on-air (0x2d-byte packets).

UDP port 50002 — status/control:
- `0a` CDJ status (track/bpm/pitch/playhead/on-air/tilt...), `29` mixer status.
- `05`/`06` media query/response; `19`/`1a` load track command/ack.
- Track metadata queries (track title/artist/waveform from other players)
  use TCP 50002 request/reply.

## Phased plan

Phase 0 — network: complete `eth_bringup` (netif + DHCP, static fallback
DHCP-server mode later for direct cable). Pure infra, testable standalone.

Phase 1 — passive observer (highest value, lowest risk):
- Bind UDP 50001: decode beat (`28`) and absolute position (`0b`) packets.
- Bind UDP 50002: decode CDJ status (`0a`) → identify tempo master, per-device
  BPM/pitch/playhead/on-air.
- Expose to `control_link`/UI: master BPM, beat phase, bar position; feed the
  existing beat-grid/loop engine and beat-jump UI from the network master when
  no local master deck is playing. P4 stays authoritative for local playback;
  DJ Link is an external sync source.
- Deliverable: P4 shows "CDJ-3000 #1 — 174.2 BPM — bar 32" on Overview while
  an actual CDJ plays on the same switch. Testable with just a CDJ/mixer.

Phase 2 — virtual CDJ:
- Send keep-alives + stage claims on 50000 (device number configurable,
  default auto-claim avoiding conflicts), emit CDJ status `0a` on 50002.
- This makes CDJs send us full status (required by some devices) and lets
  rekordbox/Afterglow-class tools see the P4 as a player.

Phase 3 — beat emission:
- Emit `28` beat packets from the playing P4 deck using its loaded ANLZ beat
  grid + current pitch, and `0b` absolute position at 30 ms. Optional tempo
  master: P4 becomes the master the CDJs can SYNC to.

Phase 4 — control integration:
- Decode sync-control `2a`, master handoff `26`/`27`, load-track `19` (map to
  P4 deck load — needs a policy decision: which deck, allow or ignore).
- Optional: TCP metadata query — pull track titles/waveform of CDJ-loaded
  tracks for display (P4 can also serve its own ANLZ data).

## Component layout (proposed)

```
firmware/main-deck-p4/components/
  dj_link_core/        packet build/parse, magic header, device registry
  dj_link_net/         lwIP UDP sockets 50000/50001/50002, task + timers
  dj_link_state/       peer status table, master election view, UI/event bridge
```

Keep packet parsing table-driven and fuzz-tested on host
(`tests/run_p4_host_tests.ps1`) — header magic, lengths, offsets per the
djl-analysis bytefields.

## Risks

- R1 lwIP socket count/memory next to USB iso audio: allocate PCBs at init,
  no per-packet allocs; measure CPU with audio running (I2S/PPA deadlines).
- R2 Broadcast flooding on club LANs: our TX volume is small; RX volume from
  4 CDJs + mixer is modest (~40 pkt/s status+beats). Drop-to-null anything
  without the magic header before deeper parse.
- R3 Fidelity rule (AGENTS.md): upstream has nothing to diverge from here —
  land as additive components; propose upstream after Phase 1 is stable.
- R4 Hostile/foreign LAN traffic: never act on a command packet without a
  validated session (device must have announced on 50000 first); settings
  toggle defaults OFF.
- R5 CDJ firmware variance (status length 284 B on newer fw; nxs2/3000
  differences): parse defensively by lenr, not fixed length.
