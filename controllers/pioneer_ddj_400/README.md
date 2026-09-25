# Pioneer DDJ-400 Controller Support for Pajoniiir

## Overview

> **v213 : le profil SD vient de la map Mixxx FLX4, pas de ce README.**
> `profile.json` / `profile.s3bin` sont dérivés de
> `controllers/pioneer_ddj_flx4/profile.json`. Ce dernier vient de
> `docs/reference/Pioneer-DDJ-FLX4.midi.xml`, une map Mixxx « based on
> DDJ-400 mapping ». Seules les entrées Smart CFX / Smart Fader, propres à la
> FLX4, ont été retirées. Le profil a été confirmé sur une DDJ-400 réelle
> (HIL v212, lignes `MIDI xx xx xx -> mapped`, par ex. `B6 0D 3F`).
> `MIDI_MAP_RESEARCH.md`, `ddj400_midi_ci.c/.h` et le tableau de différences
> ci-dessous sont **faux** (VID/PID inventés, PLAY 0x0E, 6 pads au lieu de 8) :
> ne pas s'en servir. Le VID/PID ci-dessous est corrigé depuis v213.
> Pour régénérer : copier le profil FLX4 (nom, `pid` 0x0026, sans les entrées
> `smart_*`), puis
> `python tools/controller_profile/compile_profile.py profile.json -o profile.s3bin`.
> Déploiement : `/sd/controllers/pioneer_ddj_400/profile.s3bin`.

This directory contains the MIDI Controller Interface (MIDI-CI) for the **Pioneer DDJ-400** DJ controller.

### Key Differences from DDJ-FLX4

| Feature | DDJ-400 | DDJ-FLX4 |
|---------|---------|----------|
| Hot Cues | **6** (A-F) | 8 (A-H) |
| LCD Display | ❌ No | ✅ Yes |
| Smart CFX | ❌ No | ✅ Yes |
| Pad FX | ✅ 13 effects | ✅ 14 effects |
| Loop Controls | Physical buttons | Touch pads |
| Jog Wheels | Mechanical + sensor | Platter + ring |

### LED IDs without a DDJ-400 output

`profile.json` has no output for these `led_id_t` values
(`control_link.h`). The runtime logs them once as
`LED <id> deck <d> state <s> -> no mapping` and drops them; this is expected.

| ID | `led_id_t` | FLX4 builtin | Sent by | Reason |
|---|---|---|---|---|
| 2 | `LED_BEAT` | none | UI only (`ui_status.c`) | no controller LED on either device |
| 3 | `LED_END` | none | UI only (`ui_status.c`) | no controller LED on either device |
| 41 | `LED_SMART_CFX` | `96 00` | deck_core snapshot + button, deck 0 only | DDJ-400 has no Smart CFX |
| 42 | `LED_SMART_FADER` | `96 01` | deck_core snapshot + button, deck 0 only | DDJ-400 has no Smart Fader |

All other LED IDs map to the same status/note bytes as the FLX4 builtin
table (deck 0 = `90`/`97`/`98`/`94`/`B0`, deck 1 = `91`/`99`/`9A`/`95`/`B1`).

Cross-checked against the Mixxx script `Pioneer-DDJ-400-quirx-script.js`
(quirxmode/ddj400-mixxx-mapping): PLAY/CUE `90`/`91` `0B`/`0C`, pads
`97`/`99` (shift `98`/`9A`), VU `B0`/`B1` `02`, track loaded `9F` `00`/`01`.
The script's `lights` table lists deck 2 `vuMeter` as `B0`; its VU function
really sends `B1`, which is what the profile uses.

The script scales the VU as `value * 150` (Mixxx VU 0..1), i.e. full meter
at 85 % input. Since v226 `profile.json` declares this on the `vu_meter`
output as `"scale": 150`; the JC1060 profile runtime applies it to the 0..127
level, clamped to 127 (a MIDI data byte cannot carry 150). v224-v225
hardcoded the same curve in `controller_led_runtime.c` by VID/PID. The script also sends the Pioneer SysEx
`F0 00 40 05 00 00 02 06 00 03 01 F7` at startup (control-position request);
it is declared in `profile.json` as `"init_sysex"` and sent by the generic
profile path (`controller_led_runtime_send_profile_init()`) on profile
activation, before the first LED snapshot.

### Channel FILTER and Beat FX CH SELECT (v232)

- **Channel FILTER** (`B6 17/37`, `B6 18/38`, identical to the FLX4): the P4
  runs the channel filter DSP only while Smart CFX is on, a FLX4 feature.
  The DDJ-400 has no Smart CFX button, so the knobs had no effect. On
  JC1060, since v232, a profile without a `system.smart_cfx` input makes the
  filter always live (`audio_engine_set_channel_filter_needs_smart_cfx`, set
  from the profile runtime change callback). Centre is still a bypass.
- **Beat FX CH SELECT**: the DDJ-400 switch sends `94 10` (CH1), `94 11` (CH2)
  and `94 14` (MASTER), all on status `94` (Mixxx `Pioneer-DDJ-400.midi.xml`,
  quirx `beatFxChannel`). The FLX4 `state_pair` (`94 10` + `95 11`) only
  caught CH1. `profile.json` now uses three `note_select` inputs (value on
  press only, a JC1060-only raw type, so this profile no longer loads on the
  S3 parser). MASTER maps to target 2 (CH1&CH2): the P4 has no master-bus
  Beat FX, so the effect runs per deck, pre-fader. Filter and Flanger sound
  the same as on master. Echo/Delay tails follow the channel faders instead
  of the master.
- **Firmware requirement (v233)**: raw type 8 (`note_select`) needs JC1060
  firmware v233 or later. v232 checked the parser but not the SD profile
  manager (`CPM_MAX_RAW_TYPE` stayed 7), so it rejected this 6176-byte profile
  without a log and fell back to the built-in map. Since v233 every rejection
  is logged as `ctrl_profile: profile rejected: ...`. The pre-v232 file
  (6160 bytes, no raw type 8) still loads.

## Hardware Identification

- **Vendor ID**: `0x2B73` (Pioneer DJ)
- **Product ID**: `0x0026`
- **USB Class**: MIDI Class-Compliant

## Files

- `MIDI_MAP_RESEARCH.md` - Complete MIDI mapping documentation
- `ddj400_midi_ci.c` - MIDI parser implementation
- `ddj400_midi_ci.h` - Header file
- `profile.s3bin` - S3 controller profile (generated)

## Integration Status

| Component | Status | Notes |
|-----------|--------|-------|
| MIDI Parser | ✅ Implemented | Full mapping for all controls |
| S3 Profile | ⏳ Pending | Requires binary generation |
| UI Adaptation | ⏳ Pending | Hide hot cues G/H |
| Hardware Testing | ⏳ Pending | Requires physical DDJ-400 |

## Building

Add to your S3 firmware build by including in `components/CMakeLists.txt`:

```cmake
add_subdirectory(../../controllers/pioneer_ddj_400 ddj400_midi_ci)
```

## Usage Example

```c
#include "ddj400_midi_ci.h"

// Initialize
ddj400_init();

// Parse MIDI message
uint8_t midi_msg[] = {0x90, 0x0E, 0x7F}; // Deck 1 PLAY
control_link_event_t event;
esp_err_t err = ddj400_midi_parse(midi_msg, 3, &event);

if (err == ESP_OK) {
    // Forward to control_link
    control_link_send_event(&event);
}
```

## Testing

Run host tests:
```powershell
cd tests/ddj400_midi_host
.\run_ddj400_host_tests.ps1
```

## TODO

- [ ] Generate `profile.s3bin` from XML definition
- [ ] Add unit tests for all MIDI messages
- [ ] Validate with physical hardware
- [ ] UI adaptation for 6 hot cues
- [ ] Document Pad FX assignments

## References

- [Pioneer DDJ-400 Official Page](https://www.pioneerdj.com/en/product/controllers/ddj-400/)
- [Mixxx Controller Mapping](https://github.com/mixxxdj/mixxx/tree/master/resources/controllers)
- [MIDI Implementation Chart](https://www.pioneerdj.com/en/support/article/7e31e450-155f-487d-89e8-e2682f4e04cf/)
