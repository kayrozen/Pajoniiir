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
- Status (v304, JC1060, not yet hardware-tested): bar position and the sync
  feed are done. The P4 follows the network master; taking master
  (`2a` / `26` / `27`) is Phase 4, v305.
  - Beat clock (`dj_link_table_beat_clock`):
    - the source is a confirmed status master, otherwise the player
      already followed while its beats are fresh, otherwise the newest
      beat;
    - a candidate needs a beat `0x28` no older than 2.5 beats and must not
      report "stopped" in its `0x0a`;
    - the clock is the rx time of the last beat, the beat in bar, and the
      period from the packet's track BPM ×100 and pitch. `0x0b` BPM ×10 is
      too coarse to drive the period.
  - The dj_link task hands the clock to `deck_core` on every change through
    the `beat_clock` hook. The store is a seqlock, never a wait. Both sides
    stamp with `esp_timer` ms.
  - Top bar: the master's beat in bar is lit per UI frame from that clock
    (`deck_net_clock_beat_in_bar`).
  - Sync (`deck_net_sync.c`, pure, host-tested):
    - Settings LINK SYNC (`dj_link_sync`, NVS `djlink_sync`, default OFF)
      makes a SYNC press, from the controller or the new touch SYNC button,
      follow the network master when its clock is fresh. Otherwise SYNC is
      the local BEAT SYNC, as before.
    - Every 40 ms the deck task matches the tempo (master effective BPM over
      the local grid BPM, within ±20 %) and adds a proportional phase trim:
      10 % per beat, ±1 % max, 0.004-beat deadband.
    - The pitch goes through `audio_engine_deck_set_pitch_percent`, an
      atomic store. No audio-path change, no allocation.
    - The bar is snapped by an ordinary seek on PLAY, on engage while
      playing, or when the error stays above 0.25 beat for 1 s (at most one
      snap per 2 s, then 300 ms without trim).
    - A paused deck never seeks; a held jog keeps the pitch; a stopped or
      lost master leaves the deck at its last tempo (WAIT).
    - Button colour: amber = WAIT/ALIGNING, green = LOCKED (< 0.05 beat),
      blue = local sync. A net-synced deck never reports itself as master in
      its `0x0a`.
  - Limits:
    - half/double tempo (over ±20 %) is out of reach: WAIT;
    - no output-latency compensation; the offset must be measured on
      hardware;
    - the master's own beats are timed with the jitter of its packets and of
      the dj_link task.

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
- Since v308 our two decks (v298, one MAC/IP) claim in deck order, because
  vynull and beat-link list players by number, not by arrival. Deck 1
  (`DJ_LINK_PAIR_LOW`) takes the low number of the first free pair 3/4, 2/3,
  1/2, 5/6, 4/5. Deck 2 (`DJ_LINK_PAIR_HIGH`) starts once deck 1 is active
  and takes the nearest free number above it. Alone on the network this is
  3/4, as two CDJs; the same network always gives the same numbers, also
  after a dj_link restart. A re-claim keeps the order (deck 1 below deck 2,
  deck 2 above deck 1). Only when no ordered number is free does a deck fall
  back to the 4,3,2,1,5,6 order, and the two decks still never share a
  number. A load-track `19` goes to the deck whose number is `0x40` + 1, so
  routing follows the numbers actually held (player 3 = deck 1, player 4 =
  deck 2). The numbers are not stored in NVS.
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
     Argument 1 is the sort order. Since v310 the Library sort columns set
     it: title 1, artist 2, BPM 4, key 0x0c, and 0 for the player's own order
     (CDJ codes, applied by vynull `dbserver/menuitem.go`). The protocol
     sorts ascending only, so descending reverses the list on the P4. Its
     rows are published once the list is complete. Every change lists the
     player again (new generation). Selecting another player goes back to
     its own order.
     Since v311 the Library also opens the player's playlists with
     `0x1105` `[DMST, sort 0, id, folder]` (beat-link MenuLoader, vynull
     `dbserver/playlist.go`). folder 1 lists folder `id` (0 = root): rows
     of item type `0x0001` are folders and `0x0008` playlists, and the row
     id is the folder or playlist id. folder 0 lists the tracks of playlist
     `id` in its own order. Playlists are never sorted. Folder and playlist
     rows have no metadata and are never downloaded.
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
    peer's sort. It is taken only from "title + artist" items (`0x0704`).
    vynull's default list item is `0x0d04` ("128.0 bpm - 8A"), so its rows
    carry no artist. The metadata request brings the real artist for the
    visible rows. Since v307 it is also sent first for the row being loaded,
    wherever that row is listed (for example, a network load-track off the
    visible page). The deck takes that artist at load time, or later if the
    reply comes after the load.
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
     `.PIONEER/...` for HFS+ media. v303: the NFS lookup and GETATTR run
     on every LOAD. The cached copy is kept, and nothing is read, only
     while the peer's size and mtime match the ones it was fetched with
     (`dj_link_pdb_stamp_t`, checked in the NFS open hook). v302 reused it
     for as long as the peer, IP and browse generation stayed selected, so
     a source edit under the same selection was never fetched. A server
     without an mtime (0) always refetches. A rekordbox collection peer
     (vynull's default mode, device type `0x03`) never uses `PEER.PDB`:
     its path comes from the dbserver on every LOAD.
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
  - v307: progress bars, fed by the fetch status that `ui_update()` already
    polls (non-blocking, no allocation, LVGL task only):
    - a thin bar along the bottom of the fetching row;
    - a 4 px bar in the target deck's footer, under the title and artist.
      It is dim while the export.pdb is read and in the deck colour during
      the download. It hides when the fetch ends.
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
  - Since v300, a peer track gets its waveform and beat grid from the
    peer's dbserver, not from the NFS export (a rekordbox collection source
    such as vynull has no ANLZ file to fetch). Once the audio is cached, the
    job asks the browse session (`dj_link_db_want_blob`) for:
    - wave detail `0x2904` `[dmst, id, 0]` → `0x4a02`, blob = the PWV3 body
      (1 byte per 1/150 s, bits 4:0 height, bits 7:5 whiteness);
    - beat grid `0x2204` `[dmst, id]` → `0x4602`, blob = 20-byte preamble,
      then 16 little-endian bytes per beat (u16 beat in bar 1..4, u16 BPM
      ×100, u32 time ms, 8 bytes padding).

    The replies are larger than the 4 KiB RX buffer, so the client streams
    the blob into a PSRAM buffer (wave ≤ 128 KiB, grid ≤ 4096 beats). An
    empty reply means "no data". `dj_link_anlz` writes them as
    `/sd/djlcache/<key>.EXT` (PMAI + PWV3) and `<key>.DAT` (PMAI, PPTH, PQTZ,
    PWAV = 400-column preview of the detail). The load worker parses them
    with the same `anlz_parse_dat` / `anlz_parse_ext` as a local USB track,
    so the deck gets its overview, scrolling waveform, beat grid and precise
    duration. `<key>.DAT` in the cache means the analysis is done; prune
    keeps `<key>.*`. The analysis step gives up after 10 s, or when the
    browse session fails or changes, and the track then loads without it.
  - Analysis refresh and source cues (v303, after v302 HIL showed beat grid
    and cue edits made in vynull never reaching the deck):
    - Cache keys: `dj_link_peer_track_key` is an FNV-1a of peer IP, player
      number and rekordbox id only, with no size or date. A cached
      `<key>.DAT` was never asked for again, and peer cues were never
      fetched. On local USB tracks, `track_meta_cache` is keyed by the
      DAT/EXT size and mtime, but the peer path parses the DAT directly.
    - The job now asks for wave, grid, the cue list and (if `<key>.JPG` is
      missing) the artwork on every LOAD, cache hit included, with a 4 s
      deadline when the DAT is cached (10 s otherwise). The deck is told
      the track is ready only after that, so a cache-hit load can take up to
      4 s longer when the source is slow. It all stays in the dj_link task,
      before the load, with nothing in the audio path.
    - `dj_link_anlz_commit` decides what to keep. With no cached DAT,
      whatever arrived is written. A cached DAT is replaced only when all
      three requests got a typed reply (`io.blob` `answered`) and something
      is non-empty, so a timeout or an unsupported request keeps the
      cache. If the new answer has no wave, a stale `<key>.EXT` is
      deleted.
    - Cue list: nxs2 `0x2b04` `[dmst, id, 0]` → `0x4e02`
      `[0x2705, 0, len, blob, count]`. The blob holds the entries back to
      back, little-endian:
      - u32 length (124, or 76 for legacy);
      - u16 number at `0x04` (1..8 = hot cue A..H, other = memory cue);
      - u16 type at `0x06` (2 = loop);
      - u32 time ms at `0x0c`;
      - u32 loop end at `0x20`.

      vynull's empty answer is `[0x2b04, 1, 0, 0]` with a binary tag
      declared and an int32 in its place. The client consumes that int, so
      the next request stays framed.
    - The cues are written in the DAT as two PCOB sections, hot cues
      (list 1) then memory cues (list 0), with 0x38-byte PCPT entries. The
      sections are written even when empty, but only if the cue request was
      answered. `anlz_parse_dat` sets `has_cue_lists` when it reads such a
      list.
    - Since v309 `anlz_parse_dat` keeps every memory cue of list 0, by
      time, at most the earliest 16 (`memory_cues`). Before, only the
      earliest one was kept, as the load cue. The deck walks them with
      CUE/LOOP CALL and draws them on the waveform. Edits made on the deck
      (MEMORY / DELETE) stay in P4 NVS and are never sent back to the peer.
    - Cue origin (`hot_cue_store` blob, `source_mask` / `cue_from_source`,
      same size and version): `hot_cue_store_merge_source` applies on load.
      - Pads and the CUE button set by the user stay local and always win.
      - Slots and the cue point that came from the analysis follow it, and
        are dropped when the source drops them.
      - Without cue lists, the stored cues stay as they are.
      - Blobs saved before v303 read back as all-local. Their seeded cues no
        longer follow the source until cleared: shift+pad clears a hot cue
        and shift+CUE clears the cue point, which then reseed from the
        analysis.
    - Not refreshed:
      - the cached audio file, whose key does not change;
      - `<key>.JPG` once present;
      - `<key>.VBR`, which depends only on the audio.

      A NFS LOOKUP-size check on the audio is the next step if a source
      re-exports audio under the same id.
  - Artwork (v300): the artwork id is argument 8 of a title item
    (item type `& 0xff == 0x04`), both in the list render and in the
    metadata `0x2002` reply, so the browse row carries it. After the grid,
    the job asks for artwork `0x2003` `[dmst, artwork_id]` → `0x4002`
    `[request, 0, len, JPEG]` into a 64 KiB PSRAM buffer (vynull downsizes
    covers above 32 KiB). A JPEG larger than the buffer is dropped whole.
    vynull's "not found" is `[request, 0x32, 0]` with a fourth (blob) tag
    declared but never sent; the client stops at that tag when the status
    is non-zero. The JPEG is written as `/sd/djlcache/<key>.JPG` before the
    DAT. `ui_artwork` falls back to that file under `sd_io_gate` when the
    catalog has no artwork for a deck key (deck headers only, not while
    recording), so the cover takes the local decode and cache path. A track
    whose artwork id is 0 in the browse row gets no cover.
  - Artwork fixes (v302, after v301 HIL showed no cover on the deck):
    - A cached DAT used to skip the whole step, JPG included, so tracks
      cached before v300 never got a cover. `dj_link_anlz_plan` now fetches
      the analysis only without a DAT, and the artwork alone when the DAT is
      cached but the JPG is not (replaced in v303 by
      `dj_link_anlz_commit`, below).
    - `ui_artwork` cached "no cover" for a key. A peer load now calls
      `ui_artwork_forget`, so the JPG fetched with the track is read.
    - Format: vynull re-encodes every cover with ffmpeg
      (`-c:v mjpeg -q:v 5`, 240x240). A JPEG source comes out 4:2:0 and
      decodes. A PNG source comes out `yuvj444p` with all three components
      sampled 1x2 (`0x12`). TJpgDec (the LVGL copy, which we do not patch)
      only accepts Y `0x11`/`0x21`/`0x22` with Cb/Cr `0x11`, so it returns
      `JDR_FMT3` and the deck shows no cover. `ui_artwork_jpeg_probe` names
      the reason in the warning (`sampling 12/12/12: chroma sampling`).
      Fix on the vynull side: add `-pix_fmt yuvj420p` to its two ffmpeg
      artwork commands (`analysis/artwork.go`).
  - Peer cues come from `0x2b04` since v303 (above). The plain cue list
    `0x2104` is still not used. The engine computes the duration itself when
    there is no wave detail.
  - Seek table (v302): the peer serves no usable PVBR (vynull an all-zero
    one, which we never asked for). Every peer MP3 therefore seeked by
    `seek_estimate`'s byte-linear guess, while the engine reported the
    target. On a VBR file every cue, hot cue, loop and beat jump landed
    elsewhere than the waveform and grid showed (more than 10 s off in the
    host test). The load worker now builds the table Rekordbox would have
    written (`audio_pvbr_build`). It walks the frame headers from the end
    of the ID3v2 tag, and entry k is frame `audio_pvbr_entry_frame(k, N)`,
    N = Xing count + 1. It caches the table as `/sd/djlcache/<key>.VBR`
    (`PVB1`, audio file size, 400 entries). The engine then seeks with
    `seek_pvbr` exactly as for a USB track. Without a Xing count (CBR files
    without a header) there is no table, and the estimate is exact anyway.
    The build reads the file once in 32 KiB gated chunks, and is skipped
    while recording.
  - Residual constant offset (measured, not corrected): the vynull analysis
    (`lazyAnalyze`) is decoded by ffmpeg, which drops the Xing frame and the
    encoder delay. Our minimp3 output, like Rekordbox's time base, keeps
    both. On a 20 s click MP3 the onset is at 1051 ms on our side and at
    1000 ms for ffmpeg, about 51 ms apart. vynull shifts the grid by
    +25 ms (`LossyEncoderDelayMs`) but not the waveform. So the grid can sit
    about 26 ms early and the waveform about 51 ms early on vynull-analysed
    MP3s. Tracks with imported Rekordbox ANLZ are not affected. Correct this
    on the vynull side, or with a per-source shift after HIL confirms it.
  - Risk (real CDJ): beat-link documents 19 leading bytes before the wave
    detail entries in a CDJ's `0x4a02` blob. vynull sends none. We store the
    blob as is, so on a CDJ the waveform may be shifted by 19/150 s. Check
    this on HIL before adding a skip.
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

Phase 3 — beat emission (v301, `0x28` / `0x0b` sent; tempo master still
planned):
- Each deck already broadcasts its CDJ status `0x0a` on 50002 every 200 ms
  (`DJ_LINK_STATUS_MS`, since v298, `dj_link_players_service`).
- Since v301, every deck that has a track and holds a player number also
  broadcasts on 50001:
  - absolute position `0x0b` every 30 ms (`DJ_LINK_POSITION_MS`), playing
    or paused. It carries the track length in s, the playhead in ms, the
    pitch ×100 and the effective BPM ×10 (`-1` when the BPM is unknown).
  - beat `0x28` when the playhead crosses a beat of the deck's ANLZ grid,
    while playing. It carries the beat in bar, the track BPM ×100, the raw
    pitch, and the next / second / fourth / eighth beat and next / second
    bar distances. Those distances are track time at 0 % pitch, as the
    packet defines them, and `0xffffffff` past the reported window.
- Every 100 ms the UI reports the engine playhead (`deck_state.position_ms`)
  for each deck, along with the duration, the speed and a window of 16 grid
  beats around it (`dj_link_report_beats`):
  - the speed is the engine's effective speed (pitch × jog bend), or 0
    while the scratch position is authoritative;
  - dj_link stamps each report on arrival.
- dj_link extrapolates the playhead between reports
  (`dj_link_deck_playhead`):
  - for at most 1 s;
  - clamped to the duration.
- `dj_link_session_beat` sends at most one beat per pass: the latest beat
  crossed since the last pass. A jump larger than 1 s is a seek: it re-arms
  without sending. A report slightly behind the estimate never sends a beat
  twice.
- While a joined deck has a track, the dj_link task (never an audio task)
  wakes for the next position or beat, whichever is due first. Otherwise it
  keeps its 100 ms RX wait.
- No audio-path change: everything is built from the UI's report, using the
  existing codec builders (`djlink_beat_build`, `djlink_position_build`).
- Limits:
  - the playhead is the engine position, not the audible one (no output
    latency compensation);
  - beat timing has the jitter of the dj_link task's 1 ms tick and
    scheduling;
  - beat counters, the on-air flag and tempo master (CDJs SYNCing to the
    P4) are still not sent.

Phase 4 — control integration:
- Decode sync-control `2a`, master handoff `26`/`27`, load-track `19` (map to
  P4 deck load — needs a policy decision: which deck, allow or ignore).
  v304 follows the master (Phase 1 sync); `2a` and `26`/`27` are next.
- Status (v305, JC1060, not yet hardware-tested): `2a`, `26` and `27` are
  integrated behind the same LINK SYNC switch (`dj_link_sync`, default OFF).
  - Codec: the CDJ status carries Mh (`0x9f`, `master_handoff`; built as
    `0xff` when 0). Per sync.html, byte `0x27` of `2a` and `27` is the
    sender's own number, as at `0x21`; the field names of the vendored codec
    are kept and their comments corrected.
  - `dj_link_master.c` (pure, `test_dj_link_master`) is the negotiation:
    - a deck wants master while its SYNC MASTER is set and it has a track;
    - no peer asserts master: we assert right away (a mixer is not in the
      peer table, so a mixer master is simply taken over);
    - a peer asserts master: `26` is sent unicast to its port 50001 (resent
      every 500 ms). We assert once its status names our number in Mh. After
      2 s without that the request counts as refused, and there is no retry
      until SYNC MASTER is pressed again;
    - `26` received while we assert: `27` answers from our master deck's
      number, our status keeps the master flag with Mh = requester, and once
      the requester asserts we stop and deck_core drops SYNC MASTER. After
      3 s without that we keep master;
    - a master naming one of our loaded decks in Mh unasked hands it over:
      deck_core is asked to set SYNC MASTER;
    - a peer asserting master over us without a handoff wins: we drop
      (two masters is worse than none);
    - while we assert, the beat clock handed to deck_core is invalid, so
      SYNC on our other deck syncs locally to our master deck instead of a
      peer that follows us.
  - `2a` policy: both of our players share one IP and the packet carries no
    target number, so the target is the only deck holding a player number,
    else the only loaded one, else the only playing one among the loaded.
    Anything else is refused and logged (`refused: ambiguous deck`).
    `10` = SYNC on, `20` = SYNC off, `01` = SYNC MASTER.
  - Network requests reach deck_core only as `deck_core_net_command()`
    (internal deck-queue event, never waiting, no screensaver wake); the deck
    task applies them like the buttons, so the P4 stays authoritative.
  - LINK SYNC OFF keeps the v301 behaviour: the status shows master whenever
    SYNC MASTER is set, and `2a` / `26` / `27` are only logged.
  - Not done: Syncn (`0x84`) is still sent as 0, and fader start `02` /
    on-air `03` are ignored.
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
