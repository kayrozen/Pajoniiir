# UI Work Plan — Moving Waveform Zoom Levels x0.5 / x2 (jc1060)

Status: proposal. Target: v226+ (after spikes/VU-scale batch).

## Current state (v225)

- The moving (scrolling) waveform lives in `ui_overview_motion` /
  `ui_overview_renderer` and is sized by `window_ms`.
- `window_ms` comes from `ui_overview_window_ms_from_bpm_x100_for_zoom()`:
  `beat_ms * visible_beats[zoom_step]`, clamped by
  `UI_OVERVIEW_ZOOM_WINDOW_MIN_MS` / `UI_OVERVIEW_WINDOW_MAX_MS`.
- Zoom ladder: `s_zoom_visible_beats = {4, 8, 12, 16, 24}` beats
  (`ui_overview_window.c`), default step 3 (16 beats? — verify: step index 3
  of that table = 16), adjustable via `ui_overview_zoom_delta(delta)` which is
  already wired to a controller event (`ui.c:1663`, scaled by the event
  sender).
- On zoom change, per-deck `last_wave_center_ms` is invalidated and re-blit is
  armed (3 frames) — the redraw path already handles window changes.

## Goal

Two explicit zoom levels relative to the current view, reachable from the
DDJ-400 surface:

- **x0.5** — see half the time span (twice the horizontal detail)
- **x2** — see twice the time span (overview-like detail)

## Design decision: keep one ladder, add the two levels

Do not introduce a second multiplier axis on top of the beat ladder. Instead
make the beat ladder itself contain x0.5 and x2 of the default step, so the
controller zoom event walks through them and the existing
independent-resize/invalidation path keeps working unchanged.

New ladder (default = 12 beats, x1):

```
{ 3, 6, 12, 24, 48 }        // x0.25  x0.5  x1  x2  x4
```

Notes:

- 3 and 6 beats are integers, so the beat-aligned motion math
  (`ui_overview_window_ms_from_bpm_x100_for_zoom`, beat grid) stays valid.
- 48 beats may hit `UI_OVERVIEW_WINDOW_MAX_MS` for high-BPM tracks — that
  clamp already exists and is the correct behaviour.
- 3 beats must not fall under `UI_OVERVIEW_ZOOM_WINDOW_MIN_MS` at low BPM —
  same, clamp exists. Verify the clamped step still reads sensibly (log line
  exists: `overview waveform zoom: %u beats`).

## Steps

### 1. Ladder change (`ui_overview_window.c`)

- Replace the table with `{3, 6, 12, 24, 48}`; keep
  `UI_OVERVIEW_ZOOM_STEP_COUNT` and default step (new default index = 2 =
  12 beats, preserving the current default view width).
- Confirm `UI_OVERVIEW_WINDOW_MAX_MS` / `MIN_MS` constants still give useful
  ranges at 60-200 BPM for both ends (host-side unit check possible in the
  gcc harness, like the resampler tests).

### 2. Zoom indicator

- The overview already logs zoom changes. Add a small on-screen indicator
  (x0.5 / x1 / x2 / …) near the waveform, styled like the existing overlay
  elements, visible ~2 s after a zoom change then fading out. Reuse the
  overlay pattern (`ui_overlay_map`) instead of a persistent widget.

### 3. Controller wiring (profile)

- Map the DDJ-400 zoom control to the existing zoom event: in Mixxx the
  waveform zoom is SHIFT + browse (already implemented in the Quirx script).
  Verify what our profile emits on the browse encoder press/rotate and add a
  zoom event if absent (profile.json change → recompile s3bin → SD deploy).
- Keep the `ui_update()`-only rule for applying the event in the LVGL task.

### 4. Performance guardrails

- x2 (48 beats) triples the samples per frame vs the 16-beat default. Check
  the per-frame blit cost on the JC1060 target (PROBE/HB already measures
  mix; UI cost shows up in the 5334 µs block budget):
  - if the reblit of a 48-beat window breaches the frame budget, cap the
    x2 level to the largest step that stays within budget and log it
    (`zoom step capped: window too wide`).
- The wave cache (`ui_overview_wave_cache`) is sized for the current window —
  verify its span covers 48 beats (worst case low BPM: 48 × 1000 ms =
  48 s of waveform). Extend capacity or fall back to a coarser sampling of
  the cached PCM timeline if needed.

### 5. Validation

- UI simulator e2e (`run_ui_simulator_e2e_jc1060.sh`): baselines will change
  (10-row Library batch pending as well). Review screenshots, then
  `-UpdateBaselines`.
- HIL: zoom x0.5/x1/x2 while playing on both decks — verify no frame
  overruns in HB, waveform stays beat-aligned, seek still lands on the
  correct absolute position (zoom change invalidation already covers this).

## Out of scope

- Waveform amplitude scaling (x0.5/x2 here means time, not gain).
- Per-deck independent zoom (both decks share the step, matching rekordbox).
