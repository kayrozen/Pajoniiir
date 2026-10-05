# codex/fork-improvements — evaluation snapshot 2026-10-05

Local evaluation note of upstream branch `codex/fork-improvements`
(1215 commits, head `5272bb3e`, 2026-10-05). The branch has reached the end of
its software roadmap; all packages A-L are software-verified. The remaining
critical path is JC1060/DDJ-400 hardware acceptance, which only exists here.

## Package state (from docs/FORK_IMPROVEMENTS.md at head)

| Package | Scope | State |
| --- | --- | --- |
| A | Host tests, PDB title, PQTZ downbeat | Software verified; physical NOT RUN |
| B | Seek, duration, loop resize, cues, load lock | B1-B19 on JC4880; physical NOT RUN |
| C | Playlists, artwork, PWV4 | C1-C5 on JC4880 + 800x480 sim; physical NOT RUN |
| D | S3CP v4, DDJ-400 profile | Software verified; DDJ-400 hardware NOT RUN |
| E | Board adapter, JC1060 target/BSP/CI | E1/E2 verified, CI PASS; hardware NOT RUN |
| F | UAC formats, MAIN routing, paced USB audio | Software + CI PASS; physical NOT RUN |
| G | SD idle workaround, bounce, recorder | Software verified (6 CI jobs PASS); physical NOT RUN |
| H | Product layouts 800x480 + 1024x600 | JC4880 corrective image `08996790` INSTALLED; focused startup/resources/listening + 30-min dual-loop timing PASS; full baseline/soak open |
| I | MIT djlink codec, discovery/claim/browse | CLOSED in software incl. I4b browse cache/Library bridge; physical interop NOT RUN |
| J | Media identity, cancellable download, atomic cache | CLOSED in software (manifest-last, cancel/corruption/space/admission/AB-swap PASS, SD playback survives USB/network removal); physical NOT RUN |
| K | Epoch-bound clock, sync, tempo master | CLOSED in software (donor-derived follower, no periodic seek, WAIT on source loss, sticky master, bounded handoffs); audible phase/handoff NOT RUN, sink latency uncalibrated |
| L | OTA, qualification, release candidate | Board-specific OTA candidates incl. separate JC1060 channel, 0x380000 budget; JC4880 installed w/ focused PASS; final qualification + publication open; JC1060 hardware NOT RUN |

## Lessons crossing over (their hardware phase echoed ours)

- c4912d5/6dfaf24: "preserve internal DMA heap and HTTP stack during H
  acceptance" — same pre-`usb_host_install` internal budget pitfall that
  boot-looped us in v330 (web server + mdns started too early).
- 2941121b: "Avoid periodic PSRAM heap walks during USB audio service" —
  heap polling costs USB time (our `[HEAP]` probe lesson).
- 08996790: PCM5102A DMA pacing aligned with mixer block geometry.

Soak record is honest: two interruptions logged (host monitor disappeared,
then operator-approved continuation).

## Our position

- Donor snapshot still frozen at `428b97d` (v323). Our push `69d7fec`
  (v324-v335: sinc resampler, WSOLA2 keylock, USB/MIDI pacing, download
  queue, eth web UI) is the delta to hand the maintainer — most relevant to
  J (cancel/queue = our v335) and K (compare epoch clock vs our
  deck_net_sync).
- Our HIL campaign (v297-v335, ~40 hardware-validated versions: memory cues,
  playlists, PWV4, SD/USB DMA fix, keylock AB, pacing) is the only existing
  hardware execution evidence for JC1060 + DDJ-400 — directly usable as
  evidence for packages E/F/G/I4b and for the L JC1060 candidate when signed.
- Next actionable step for us: flash and exercise the package L JC1060 OTA
  candidate once it is published/signed.

## Standing integration constraints (theirs, worth keeping here)

- Never replace the current PCM timeline with the fork's older engine.
- Keep slow storage/network/JPEG work out of audio and LVGL draw callbacks.
- Bind async results to track identity, source generation, request epoch.
- Keep ESP-IDF 6.0.2, pinned USB fixes, 0x380000 application budget.
- Preserve `main-deck-p4` OTA identity; `main-deck-jc1060` for this board.
