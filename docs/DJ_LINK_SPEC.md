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
- Status (v246, JC1060, not yet hardware-tested): observe-only subset done:
  `components/dj_link` (raw lwIP UDP PCBs bound to the Ethernet netif,
  prio-1 task on core 1, 6-peer table) + vendored codec `components/djlink`;
  Settings switch `dj_link_enable` (NVS `djlink_en`, default OFF) and status
  line. Not yet done: bar position, control_link/beat-grid feed, Overview.
  Without Phase 2 keep-alives the CDJs may send no `0a` status, so the master
  is then inferred from beats (shown with `?`). Broadcasts are only heard on
  the P4's own subnet (no AutoIP / link-local yet).

Phase 2 — virtual CDJ:
- Send keep-alives + stage claims on 50000 (device number configurable,
  default auto-claim avoiding conflicts), emit CDJ status `0a` on 50002.
- This makes CDJs send us full status (required by some devices) and lets
  rekordbox/Afterglow-class tools see the P4 as a player.
- Status (v247, JC1060, not yet hardware-tested): pure, host-tested
  `dj_link_session.c`. Join sequence `0a`/`00`/`02`/`04` (3x each at 300 ms),
  auto number in the order 4,3,2,1,5,6, skipping numbers heard in the last
  5 s. We re-claim on `08` or on a keep-alive carrying our number; we never
  defend it. Device type CDJ, keep-alive `06` every 2.0 s.
- Media query `05` gets a `06` reply, unicast only: our USB slot,
  "PAJONIIIR LIBRARY", track count, 500 ms per-querier limit, no reply when
  the library is empty. The advertised media cannot be browsed yet: we do not
  serve our own NFS export or dbserver (the v249 NFS code is a client only).
- Incoming load-track `19` is accepted only if it is unicast to us after we
  joined, and the source is our own USB (`0x28` = our number, `0x29` = USB).
  Loads from another player's slot are refused and logged (v248+).
  `ui_update()` loads into the non-playing deck, refuses when both decks
  play, and sends `1a` only for accepted loads. The UI shows
  `D<n> LOADED FROM #<sender>`.
- Not done yet: CDJ status `0a`, outgoing `19`, serving our own dbserver.
- Same rules as v246: the toggle defaults OFF (no packets sent when OFF),
  Ethernet only.

Phase 2b — browse peer libraries (dbserver client, peer → us):
- Status (v248, JC1060, host-tested, no CDJ hardware test yet): pure client
  `dj_link_db.c`, which does no I/O itself. It builds every request with the
  vendored `djlink_dbserver` codec. In the firmware it runs over one raw lwIP
  TCP PCB bound to the Ethernet netif. That PCB is driven only from the
  dj_link task through `tcpip_exec`.
- Session sequence:
  1. Connect to peer:12523 and send the `RemoteDBServer` port query. The peer
     answers with a u16 port; then close.
  2. Connect to that port; the greeting `11 00 00 00 01` is echoed back.
  3. Context setup with our player number.
  4. All-tracks menu `0x1004` (DMST = `our<<24 | 1<<16 | slot<<8 | 1`, USB
     slot only) → `0x4000` with the item count.
  5. Render `0x3000` in batches of 64 (`0x4001` / `0x4101` items / `0x4201`).
     Each item gives rekordbox id, title and artist.
  6. Duration and BPM come from a metadata request `0x2002` per row. It is sent
     only for the rows visible on the current Library page.
- Session rules:
  - A session starts only when the operator picks the peer in the Library
    (SOURCE button). There is no polling.
  - One connection and one request in flight at a time.
  - 3 s timeout per step; the session closes after 20 s idle.
  - Errors are shown in the Library source line: `TIMEOUT <PHASE>`,
    `NO DB SERVER`, `BAD REPLY`, `CONNECTION LOST`.
- Memory: 64 KiB TCP ring plus a 2000-row cache (about 64 + 320 KiB of PSRAM).
  It is allocated only while a peer is browsed and freed on LOCAL or OFF.
  Nothing is allocated in the audio path.
- UI: the SOURCE button cycles LOCAL → `USB <name> #n` for each active player.
  - The UI reads the dj_link row cache under a spinlock and never waits on the
    network.
  - Sorting is local-only.
  - The KEY column shows an amber `META` badge.
  - The Library falls back to LOCAL when the player leaves or DJ Link is
    switched off.
- Load (v248): every peer track was **metadata only** and the load was refused.
  Since v249 a peer browsed through the dbserver is marked
  `DJ_LINK_PEER_AUDIO_NFS`, and LOAD downloads the file first (see below).
  `dj_link_peer_load_check()` still refuses rows whose audio is not reachable
  (`METADATA ONLY - NO AUDIO`) before any deck or audio state is touched.
- Caveats:
  - Our auto-picked number can be 5 or 6 when 1–4 are taken. Real CDJs may
    refuse dbserver queries from numbers above 4 (djl-analysis); expect
    `BAD REPLY` then.
  - The artist in the list is the menu's second label and depends on the
    peer's sort. The metadata request overwrites it with the real artist for
    the visible rows.
  - SD / rekordbox-collection slots and playlists are not browsed yet.

Audio for peer tracks (v249 decision). The dbserver never serves audio. The
two options:

1. **NFSv2 client, recommended (chosen, implemented in v249).** CDJs export
   each mounted medium over NFS (portmapper → mount `/C/` for USB, `/B/` for
   SD → NFS READ; names are UTF-16LE). This is how beat-link / crate-digger
   fetch `export.pdb`, ANLZ files and audio from real players. The audio path
   is not taken from the dbserver: v249 downloads the peer's `export.pdb` and
   resolves the rekordbox id to its file path there.
   - Implementation: UDP ONC-RPC (portmapper GETPORT, MOUNT MNT, NFS LOOKUP
     per path element, then READ in ≤ 8 KiB chunks). It runs in the dj_link
     task on one pre-allocated UDP PCB and streams into a PSRAM / SD cache
     file.
   - The deck loads from that cache through the existing local load path once
     the file is complete. Audio never reads from the network.
   - Works with real CDJ-2000NXS2 / CDJ-3000 and with other Pajoniiirs, if we
     also export our USB read-only later.
   - Cost: about 1 kLoC of pure, host-testable XDR/RPC code, and a mock NFS
     peer in `test/host`.
   - Risks: read-only, no auth (AUTH_UNIX uid 0), UDP retries on a busy LAN,
     and a large file (about 10 MB for a 5-minute MP3, about 50 MB for WAV)
     takes seconds to fetch. The load must show progress and stay cancellable.
2. **Custom Pajoniiir-to-Pajoniiir transfer** (for example HTTP range GET on
   our existing OTA HTTP server). It is simpler (TCP, one request) but only
   works between Pajoniiirs, never with a real CDJ. It would also revive the
   removed custom "CDJ Link" idea that this spec warns about.

Recommendation for v249: an NFSv2 read-only client, fetch-to-cache and then
load, never streaming into the deck. Serving our own USB over NFS (so CDJs can
load our tracks) stays a separate, later step (v250+).

Phase 2c — peer track download (NFS client, peer → us):
- Status (v249, JC1060, host-tested against a mock NFS peer, no CDJ hardware
  test yet).
- Library: `components/djlink` 0.3.0 adds `djlink_nfs.c` / `djlink/nfs.h`. It
  is a pure NFSv2 read-only client that does no I/O itself:
  - ONC-RPC over UDP with AUTH_UNIX uid 0.
  - Sequence: portmapper GETPORT (MOUNT, then NFS) → MNT → LOOKUP per path
    element → READ.
  - Strict 32-byte handles, a window of READs in flight, and retransmit on
    timeout.
  - Progress and the file bytes go to caller callbacks.
  - The mock peer in `components/djlink/test/host` covers:
    - GETPORT, MNT and multi-level LOOKUP;
    - multi-chunk READ with dropped packets;
    - a file larger than 1 MiB;
    - an unknown export.
- Firmware: `dj_link.c` runs the fetch job in the dj_link task only. The job
  uses one UDP PCB on an ephemeral port, bound to the Ethernet netif and
  driven through `tcpip_exec`.
  - All its buffers come from PSRAM and are allocated per job.
  - Nothing runs in LVGL or in the audio path.
- Steps of one LOAD:
  1. The job downloads `PIONEER/rekordbox/export.pdb` from the peer's USB
     export `/C/` to `/sd/djlcache/PEER.PDB`. It falls back to
     `.PIONEER/...` for HFS+ media. The file is reused while the same peer,
     IP and browse generation stay selected.
  2. `dj_link_pdb_find_track()` walks the Tracks table page by page to find
     the file path.
  3. The audio file downloads to `FETCH.TMP`. It is written under
     `sd_io_gate` in 32 KiB batches, then renamed to
     `/sd/djlcache/<key>.<EXT>`. The extension is kept because the engine
     picks the codec from it.
  4. Once the file is complete, `ui_update()` loads it through the existing
     local load path. A file already in the cache is loaded without
     downloading.
- Cache pruning: before each download, the cache keeps only `PEER.PDB`, the
  current track and the files loaded on the decks.
- UI:
  - The source line shows `READING DB NN%`, then `DOWNLOAD NN%`.
  - The fetching row shows `DB` or `NN%` in place of `NET`.
  - The SOURCE button becomes `CANCEL` while a fetch runs. Cancelling
    deletes the partial file.
  - After loading, the UI shows `D<n> LOADED (DJ LINK)`.
  - Errors appear in the source line, for example `PLAYER NOT ON THE NETWORK`,
    `NO SD CARD`, `RECORDING TO SD`, `TRACK NOT IN EXPORT.PDB`, `LOCAL USB
    NEEDED`, or the NFS error text.
- Caveats:
  - The engine's reads are gated by the local USB media gate, so a cached peer
    file loads only while a local USB is present (`LOCAL USB NEEDED`).
  - READs are 1024 bytes because lwIP has no IP reassembly here. A 10 MB MP3
    therefore needs about 10 000 round trips, sent as a small window of
    requests in flight.
  - The first LOAD after a new browse selection downloads `export.pdb` first,
    which is several MB on large libraries.
  - Peer tracks get no ANLZ, waveform, beat grid or PVBR. The engine computes
    the duration itself.
  - The SD card is required, and the download is refused while the recorder
    writes to it.
- Hardware tests still needed with the borrowed CDJ:
  1. GETPORT and MNT `/C/` succeed on the real firmware (NXS2 and/or 3000).
  2. `export.pdb` downloads, then the rekordbox id resolves to its file path,
     including non-ASCII names (UTF-16LE LOOKUP).
  3. Download MP3, WAV and AIFF / FLAC files of about 10–50 MB. Measure the
     throughput and retransmits with both decks playing, and check for no
     audio dropouts on the P4.
  4. Cancel in the middle of a download, and unplug the CDJ's USB during a
     download.
  5. Load a track from the cache (cache hit), and load after changing the
     browse selection.
  6. Test an HFS+ formatted stick (`.PIONEER`).
  7. Check the player-number caveat above: we may take number 5 or 6.

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
- R6 NFS peer download (v249): UDP READ traffic on the shared Ethernet while
  both decks play. Buffers are per-job PSRAM, SD writes go through
  `sd_io_gate` in 32 KiB batches, and the engine only ever reads the finished
  local file. CPU and dropout impact are unmeasured until the CDJ HIL test.
