# Jog Wheel Modes — VINYL vs CDJ

Status: reference (2026-09-26), branch `master` (JC1060 fork).
Context: the operator noted that `VINYL_SCRATCH_PLAN.md` describes VINYL-mode
behavior only, but Pioneer controllers also expose a distinct **CDJ mode**.
This document records the differences and what each mode implies for the
Pajoniiir jog implementation.

## The two modes (Pioneer DDJ/CDJ convention)

| Aspect | VINYL mode | CDJ mode |
|---|---|---|
| Touch on platter **top** | Stops the track instantly (grab), acts as the scratch surface | Ignored for transport — touching the top does nothing |
| Platter top rotation while touched | **Scratch**: absolute, bidirectional, position follows the hand; release resumes or stays paused (per design) | n/a (top is inert) |
| Platter/side ring rotation while playing | Nudge (pitch bend) — but most DJs use the side ring only | **Pitch bend** from the whole wheel: temporary speed up / slow down, springs back to pitch |
| Platter rotation while paused | Frame-scrub (seek with the hand, on touch) | Nudge/seek in fine steps, no absolute grab |
| Typical use | Scratch routines, turntablist feel | Beatmatching nudges, fine adjustments — "not super sensitive", forgiving |
| Sensitivity | High (1:1 absolute on touch) | Low (rate-based bend), small errors less audible |
| Hardware on DDJ-400 | No dedicated VINYL button; VINYL behavior is the default when JOG_TOUCH is active (per our mapping) | Default rekordbox mode on many controllers; on DDJ-400 it is the mode when touch-scratch is disabled |

Community consensus sources: Digital DJ Tips ("CDJ mode allows you to use the
whole jogwheel to pitch bend... Vinyl mode works more like an old-[turntable]"),
vibesdj.io ("vinyl mode lets pressing the top stop playback and enables
scratching; in CDJ mode the top behaves..."), r/Beatmatch threads (CDJ mode =
whole platter is a nudge wheel; vinyl mode = top scratches, side ring nudges).

## What our firmware implements today

`VINYL_SCRATCH_PLAN.md` and the deck/audio code implement **VINYL mode**:
- `JOG_TOUCH` (platter top) gates scratch: touch = grab + absolute position,
  release = stay where released (v266) or resume per the scratch state machine.
- Side ring while playing = `JOG_BEND` pitch-bend nudge (`audio_engine_deck_jog_nudge`).
- Jog ticks while paused **without** touch are ignored
  ("jog +N ignored: paused, platter not touched").

That is coherent: our default IS vinyl mode.

## What CDJ mode would change (not implemented)

1. Top touch produces no grab and no stop — the transport keeps running.
2. Rotation (whole wheel, no touch needed) while playing = pitch bend only
   (no absolute scrub).
3. Rotation while paused = fine nudging (small seek steps), not absolute scrub.
4. The 1:1 hand-position coupling disappears — latency/desync complaints
   related to touch-scrub don't apply in this mode.

Implementation sketch (if requested later): a per-deck mode flag in
`app_settings` (persisted), routed through control_link like the other deck
commands; `JOG_TOUCH` events ignored in CDJ mode; `JOG_BEND` gains applied
regardless of touch; paused rotation maps to bounded fine-seek steps instead of
the scratch timeline. No audio-path changes: bend already exists (nudge), and
paused nudges can reuse the seek path with rate limiting (v265 coalescing).

## Related notes

- The v265/v266 scratch fixes (seek coalescing, stay-where-released) are
  VINYL-mode-only; CDJ mode never enters the scratch timeline.
- The DDJ-400 physical behavior (no VINYL button) means the mode on the
  controller side is fixed; a Pajoniiir-side toggle in Settings would be the
  way to expose CDJ mode.
