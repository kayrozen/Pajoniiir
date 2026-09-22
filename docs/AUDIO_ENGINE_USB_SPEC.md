# Audio Engine Spec — USB Audio Out (JC1060 / DDJ-400)

Status: DRAFT v2. Target integration: `firmware/main-deck-jc1060` on branch
`codex/ddj400-jc1060-integration`. Supersedes the earlier mixed I2S/USB
topology sketch. Evidence base: `upstream/master` (`dvucinozd/Pajoniiir`,
branch `feat/p4-dual-usb-host`, merged) which already runs a P4-only dual USB
host system with no S3.

## 1. Decisions (locked)

| # | Decision | Rationale |
| --- | --- | --- |
| D1 | **No S3.** The P4 is the direct USB host of the DDJ-400 (MIDI + UAC audio). | Upstream master already ships this topology (`peripheral_map` dual-port, no `control-board-s3`). Removes control_link, monitor_pcm_link, and the inter-board I2S hop entirely. |
| D2 | **ESP-IDF USB Host Library, not TinyUSB.** One `usb_host_install` owning both ports via `peripheral_map = BIT(0)|BIT(1)`. | Upstream proof. Also avoids the mixed-stack (TinyUSB-HS + HostLib-FS) failure mode documented in `docs/research/P4_OTG_FS_host_report.md`; single-stack dual port re-opens MSC on the second port. |
| D3 | **No I2S codec at all** on the JC1060 product path: no ES8311, no PCM5102A, no monitor link. Master + headphones both go out over USB Audio Class. | User decision. Frees both I2S units. |
| D4 | **Stream format first pass: UAC1 alt 1, 4ch × 16-bit @ 44.1 kHz.** Alt 2 (4ch × 24-bit S24_3LE) is a follow-up extension. | Matches upstream `controller_usb_audio_stream` ("44100 Hz 4ch/16-bit"). DDJ-400 is 44.1 kHz only; no SET_CUR(SAMPLING_FREQ); no Feature Unit (no USB volume/mute). |
| D5 | **Channel mapping: ch 1-2 = master out, ch 3-4 = phones.** | Mixxx manual + hw-verified for the DDJ family; upstream packetizer already interleaves `master_samples` + `headphone_samples`. |
| D6 | **Output sample rate is fixed at 44100 Hz.** Every deck resamples (`source_rate / 44100`) on top of pitch, as the existing mixer already does. | DDJ-400 endpoint has no rate control. |

## 2. Hardware context (JC1060P470C)

- Port FS (USBB2) is wired DIRECTLY to the P4 USB1 PHY (USB1P1_P/N) — OTG-FS
  host capable. Port HS (USB3) is the dedicated OTG-HS PHY — full DWC2 host.
- Two OTG ports, like the JC4880 upstream board. Port assignment TBD by bench
  test (section 9, J0): controller on HS, media stick on FS is the working
  hypothesis (DDJ-400 is a High-Speed composite device: bulk 512 MIDI needs HS;
  upstream put storage on HS, but on JC1060 the FS port is the one proven to
  enumerate only in single-stack installs — which is now the plan).
- Power: the DDJ-400 must be externally powered or fed from a supply that
  tolerates its inrush; a brownout on the 5 V rail resets the whole P4 (known
  failure signature, see skill notes).
- I2S units 0 and 1 become free (unit 2 dead on eco2 silicon). Reserve them
  for a possible future booth/recording DAC — out of scope for this spec.

## 3. USB device profile: Pioneer DDJ-400 (2b73:0026)

Composite High-Speed device:

| Interface | Class | Content |
| --- | --- | --- |
| 0 | Audio Control | EP_GENERAL bmAttributes=0x00 → no sampling-freq control, no Feature Unit |
| 1 | Audio Streaming OUT | alt 1 = 4ch × 16-bit 44.1 kHz; alt 2 = 4ch × 24-bit S24_3LE 44.1 kHz |
| 3 | MIDI streaming | bulk 512 in + out |
| 4 | HID | unused |

- MPS 576 on the iso EP = 4ch × 3B × ~44.1 frames/ms (alt 2 sizing); 16-bit alt
  needs 4 × 2 × 45 = 360 B/ms.
- 44.1 kHz → 44/45 frames per 1 ms packet (45 once every 10 packets); each
  `isoc_packet_desc[i].num_bytes` must be set individually plus
  `transfer->num_bytes`.
- A latched bad stream state silences the controller for every host until
  physical replug — the lifecycle code must treat replug/power-cycle as a
  first-class recovery, not a code retry.
- `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=512` is upstream's setting; the
  DDJ-400 config descriptor must be checked against it at bring-up (the FLX4
  needed 512; composite DJ controllers have needed up to 1024 elsewhere —
  verify, don't assume).

## 4. Signal chain

Unchanged from the current `audio_engine` (all P4-owned, CPU0):

```
per deck:
  bounded compressed cache → decoder (MP3/WAV/FLAC) → PCM ring
  → resampler (pitch × source_rate/44100) → TRIM/pregain → 3-band EQ
  → channel filter → Pad FX → Beat FX (filter/echo/flanger/delay)

mix:
  channel fader → crossfader → two-deck sum

master bus (ch 1-2):
  master volume × master trim → MAIN soft-knee limiter → master_out[]

headphone bus (ch 3-4):
  PFL branch (post-TRIM/post-DSP, pre-fader) → headphone mix (cue↔master)
  → headphone level → hp_out[]
```

Output consumer change only:

```
OLD (JC4880):  i2s_channel_write(PCM5102A MAIN)  paces the loop
               monitor_pcm_link → S3 → FLX4 USB (cue)
NEW (JC1060):  controller_usb_audio_stream_write(master_out, hp_out,
                                             frames, 44100)
               one 4-channel interleaved iso stream to the DDJ-400
```

- Keep the existing `audio_output_sink` bounded-write discipline (bounded
  calls, short-write resume, per-sink stats) around the USB write.
- The output task is paced by iso URB completion instead of I2S DMA. Keep the
  phase-timing instrumentation (`phase_main_max_us` becomes the USB write
  phase; add `phase_usb_*` naming only if a rename is cheap).
- Recorder tap (`phase_push_max_us`) stays on the master bus, unchanged
  (recorder remains compiled out by default).

## 5. Components

### 5.1 Ported from upstream (large parts reusable, FLX4/BSP coupling to strip)

| Component | Port scope |
| --- | --- |
| `usb_host_manager` (+ `usb_host_recovery_arbiter`, `usb_host_topology`) | Near-direct port. Single owner of the Host Library, dual `peripheral_map`, root-port power cycling after soft reset, coalesced recovery requests, root-port topology matching. |
| `controller_usb_host` + `usb_midi_codec` (+ `controller_midi_out_gate`, `controller_usb_recovery_gate`) | MIDI host: claim interface, bulk IN 512 polling via transfer callbacks, 4-byte USB-MIDI event packet decode (CIN nibble), MIDI OUT queue with backlog policy (drop VU-class traffic, retain state LEDs). Retarget device matching from FLX4 VID/PID 2b73:0045 to DDJ-400 2b73:0026. |
| `controller_usb_audio` (`controller_usb_audio_stream`, `flx4_uac_descriptors`, `flx4_uac_packetizer`, `controller_audio_ring`, `controller_audio_resampler`) | Near-direct port; rename-to-generic optional. Descriptor parser walks the class-specific AS descriptors (read bSubframeSize/bBitResolution/bSamFreqType — never assume); packetizer implements Q-style 44/45 frame accumulation; stream owns alt-setting claim, iso URB queue (bounded transfers in flight, per-packet resubmit, per-packet loss tolerance), endpoint-halt recovery, quiesce before free. Clock adaptation via `clock_trimmed_frames`/`clock_duplicated_frames`. |
| `controller_runtime` / `controller_led_runtime` / `p4_local_controller` | Replace S3 control_link: P4 consumes DDJ-400 MIDI directly, owns LED state, drives MIDI OUT. Detail design is a separate spec (controller integration), but the seam is named here so the audio spec does not grow a control_link dependency. |

### 5.2 New / modified in this tree

- `audio_engine`: replace the PCM5102A + monitor-link sinks with the single
  USB write. Delete `CONFIG_BSP_PCM5102A_MAIN_OUT`, `CONFIG_BSP_ES8311_MONITOR`,
  `CONFIG_MONITOR_PCM_LINK_I2S_UNIT` from the JC1060 defaults. Output rate
  constant 44100.
- `bsp` (JC1060): remove `bsp_audio_init()` I2S setup from the product path
  (display/touch/SD stay).
- New `main-deck-jc1060` sdkconfig.defaults:
  `CONFIG_USB_HOST_HUBS_SUPPORTED=y`,
  `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=512` (verify), no I2S audio
  options, SPIRAM options carried from the JC4880 baseline (XIP from PSRAM is
  mandatory — flash writes during OTA must not starve the iso deadline).

## 6. Timing and buffering

| Item | Value |
| --- | --- |
| Iso packet interval | 1 ms |
| Frames per packet (44.1 kHz) | 44/45 alternating |
| USB stream ring | upstream-sized ring in `controller_audio_ring` (capacity + high-water in stats); target ≈ 20 ms |
| Transfers in flight | bounded set (`STREAM_TRANSFER_COUNT` upstream), several × multi-packet URBs |
| Audio block | 256 frames ≈ 5.8 ms @ 44.1 kHz |
| End-to-end latency estimate | ~25-30 ms |
| Pacing | iso completion callbacks; keep the "one real delay after N busy blocks" idle-window rule (dual keylock WDT evidence) |

Clock adaptation: the DDJ-400 endpoint clock and the P4's crystal are
asynchronous. The upstream stream counts trimmed/duplicated frames — keep that
mechanism; it is what keeps a multi-hour soak from drifting the ring into
overrun/underrun.

## 7. Risks and mitigations

| # | Risk | Mitigation |
| --- | --- | --- |
| R1 | FS port (USB1 PHY) never enumerates in the split-stack experiments; dual-port single-install is upstream-proven but not yet proven on the JC1060 board. | J0 spike first: single `usb_host_install(BIT(0)|BIT(1))`, enumerate stick + DDJ-400 on both port assignments. Do not start porting before J0 passes. |
| R2 | DDJ-400 composite descriptor larger than control-transfer max. | Measure descriptor length at bring-up; raise `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` to 1024 only if measured. |
| R3 | Latched broken stream state after a failed iso start. | Recovery = endpoint halt + quiesce + device replug / root-port cycle via the arbiter; never retry-submit into a faulted stream forever. |
| R4 | 48 kHz tracks on a 44.1 kHz-only output. | Already solved: per-deck resampler folds `source/output` into the step; host regression exists for 48k-on-44.1k. |
| R5 | OTA download (flash writes) starving the iso deadline. | `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` is mandatory on this target (proven fix on JC1060 v142+). Verify it lands in sdkconfig after each reconfigure. |
| R6 | MIDI + audio concurrent FIFO pressure on DWC2. | Upstream runs exactly this combination without a custom FIFO split on the same silicon family. If EP allocation fails (`ESP_ERR_NOT_SUPPORTED`), apply the documented custom `fifo_settings_custom` split (rx=130 / nptx=132 / ptx=148) as fallback — measure, don't guess. |
| R7 | Brownout on controller attach. | Powered/qualified supply for the DDJ-400; brownout-detector differential test before blaming drivers. |
| R8 | LVGL/DSI contention with the audio task. | Keep audio on CPU0, LVGL on CPU1 (existing rule); keep the phase-timing diagnostics to attribute late blocks. |

## 8. Deleted paths (JC1060 product build)

- `control_link` (UART 0xA5/0xA6), S3 heartbeat/LED relay, controller-profile
  0xA6 transfer machinery
- `monitor_pcm_link` (P4→S3 I2S) and `p4_audio_link`/`flx4_usb_audio` (S3)
- ES8311 driver, PCM5102A I2S sink, speaker PA path
- `CONFIG_DDJ_FLX4_*` S3-side options

These stay alive for the JC4880/FLX4 product line; the deletion is scoped to
the JC1060 target's component/CMake selection only.

## 9. Validation milestones

| Gate | Content |
| --- | --- |
| J0 | Port-assignment spike: single dual-port `usb_host_install`; stick + DDJ-400 enumerate (record which port hosts what, descriptor lengths, FIFO allocation result) |
| J1 | MIDI bulk IN/OUT: notes reach `deck_core` semantics; MIDI OUT queue delivers state LEDs; 30 min stable with audio idle |
| J2 | UAC iso OUT: test tone audible on ch 1-2 (master) and ch 3-4 (phones) of the DDJ-400; stats show 0 underrun/overrun, streaming=true |
| J3 | 4ch interleaved master + phones simultaneously, headphone-mix/level knobs acting on the right channels |
| J4 | audio_engine integration: dual-deck MP3 playback (mixed 44.1/48 kHz sources) through the USB stream; late-block counters clean |
| J5 | Soak: 30 min dual-deck, Master Tempo + FX active, no WDT, no ring drift (`clock_trimmed/duplicated` bounded), MIDI responsive throughout |
| J6 | Lifecycle: hot-plug controller, replug after fault, soft-reset + root-port cycle, OTA download while playing (XIP proof) |
| J7 | (Follow-up) alt 2 24-bit S24_3LE path, if 16-bit acceptance shows noise-floor or level issues |

Listening acceptance (operator) is required at J3 and J5; counters alone do not
close those gates.

## 10. Open questions

- Q1: which physical port (FS vs HS) hosts the DDJ-400 on the JC1060 harness?
  Decided by J0, not by preference.
- Q2: does the DDJ-400 16-bit alt actually pass audio (the FLX4-family note in
  the skill reference says only the 24-bit alt was kernel-confirmed on PC)? J2
  answers it on real hardware; if 16-bit is dead, switch the first pass to
  alt 2 S24_3LE (int16 << 8 conversion, 12 B/frame).
- Q3: does the JC1060 keep the SD slot as the media source, or does MSC move
  to the freed USB port? Interaction with J0.
