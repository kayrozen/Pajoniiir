# Rekordbox Playlists — Integration Plan (jc1060)

Status: proposal, not started. Owner: agent + operator review.

## Goal

Browse and load Rekordbox playlists from the USB drive the same way tracks are
browsed today: a Playlists category in the Library UI, playlist selection, then
track rows inside the playlist. Playback state stays deck-owned; the playlist
data is read-only metadata, resolved once at library load.

## Current state (v225)

- `components/library/rekordbox_pdb.c` walks the PDB and parses the **Tracks**,
  **Artists**, **Albums** and **Keys** tables (generic page-chain walker in
  `walk_table()`), then frees name tables after `build_index`.
- `library.c` holds a compact track index (`uint16_t` row order, `library_sort`)
  and serves the UI through `ui_library.c` (paginated 8-row table).
- The PDB header already exposes `num_tables` + per-table
  `{type, first_page, last_page}` — playlists need no new file access path.

## Phase 0 — Empirical table discovery (small, do first)

PDB table type IDs for playlists are not in our code yet (candidate values
`0x39` (Playlist) / `0x29`? seen in RE sources; verify against our real
`export.pdb`).

1. Add a debug dump in `walk_table()` behind a build flag: table type, page
   count, first row bytes.
2. Load one USB drive with a handful of playlists; capture the dump over the
   HIL serial log.
3. Confirm: playlist-list table, playlist-content/entries table, row layout
   (row `id`, `parent_id`, `seq`, `content_id` / track pointer fields).

Deliverable: documented type IDs + row offsets in this file (replaces the
candidates above). No firmware behaviour change.

## Phase 1 — PDB parsing (`rekordbox_pdb.c`)

1. Parse the **Playlist** table: playlist id, name (DeviceSQL string via the
   existing name-offset machinery), seq (order of playlists in the browser).
2. Parse the **PlaylistContent** table: `(playlist_id, seq, content_id)`.
   content_id joins to the existing Tracks row id.
3. Data model: keep it compact like the track index —
   - `playlist_t { uint32 id; uint32 name_off; uint16 track_count; }`
   - `uint32_t playlist_track_ids[]` concatenated, or a per-playlist page
     reference to avoid materialising all entries when a USB drive has
     thousands.
4. Memory budget: playlists must survive the same tight internal-RAM pool that
   forced the loader stack to PSRAM (v207). Prefer PSRAM allocation for
   playlist arrays; internal RAM only for the browse window.

Deliverable: `pdb_playlist_count()`, `pdb_playlist_get(i, &meta)`,
`pdb_playlist_tracks(id, out_ids, max)` behind the existing PDB interface.

## Phase 2 — Library index (`library.c`)

1. Extend `library_init()` build to also collect playlists (optional: build
   only when at least one exists).
2. New accessors mirroring the track API: `library_playlist_count()`,
   `library_playlist_name(i)`, `library_playlist_track_rows(i)` → row indices
   into the existing compact track store (so sorting, ANLZ and waveform paths
   are untouched).
3. Edge cases: playlist entries pointing at missing tracks (skip + count),
   empty playlists, duplicate entries (keep order), Folder-type playlist rows
   ( Rekordbox "folders" are parent rows — either show them or flatten;
   decide after Phase 0 dump).

## Phase 3 — UI (`ui_library.c`)

1. Add a **Playlists** category to the Library browse model (same branch as
   ARTISTS/ALBUMS if one exists, else as a sort/category entry).
2. Two-level browse: playlist list → track rows of that playlist. Reuse the
   paginated table (8 rows today; 10 planned once the page cache moves to
   PSRAM — see v226 NO_MEM regression); no new widgets.
3. Load from a playlist row uses the existing load path (track row → deck).
4. Keep `ui_update()`-only rule: playlist navigation commands must be applied
   from the LVGL task like all other controller browse commands.

## Phase 4 — Controller mapping

1. Map the DDJ-400 browsing controls to the new category (existing LOAD/BROWSE
   events; check what the profile assigns to the PLAYLIST navigation button
   (`BROWSE` encoder press) in the Mixxx XML).
2. Profile change if needed: add the browse-category event to
   `controllers/pioneer_ddj_400/profile.json`, recompile `s3bin`, redeploy SD.

## Phase 5 — Validation

1. Host test: parse a real `export.pdb` fixture with playlists (add a small
   fixture under `tests/`, parse with the gcc harness used for the resampler
   tests) — assert playlist count, names, and track id sequences.
2. HIL: load 52-track drive with N playlists; verify counts, order, entry into
   playlist, load from playlist, no regressions on the plain track browser.
3. Memory check: heap snapshot before/after load with a large playlist set
   (worst case: drive with hundreds of playlists).

## Non-goals (this pass)

- Editing/creating playlists (read-only).
- History playlists (separate tables, later).
- Album/Genre/Artist categories (same pattern, follow-up).
- Smart Playlists (rekordbox stores criteria elsewhere; out of scope).

## Suggested order

Phase 0 → 1 → 2 → 5 (host) → 3 → 4 → 5 (HIL). Phases 1+2 are one
reviewable unit; UI can land behind the category only after the index is
trusted.
