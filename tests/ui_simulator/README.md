# Pajoniiir LVGL UI simulator E2E gate

This gate builds the real P4 LVGL UI against a pinned upstream LVGL commit,
runs it on a headless 800x480 framebuffer and drives navigation through the
actual LVGL button callbacks. No P4, S3, FLX4, SDL window or media device is
required.

Covered screenshots:

- Overview with Deck 1 selected;
- Overview after the scripted Deck 2 selection;
- Library with deterministic track fixtures;
- Hot Cues;
- Settings;
- idle screensaver;
- Settings restored after dismissing the screensaver.

The reference is a SHA-256 manifest over the complete RGB framebuffer. A
one-pixel change therefore fails the gate and leaves the generated PPM captures
under `.cache/ui_simulator/screenshots` for review.

Run:

```bash
./tests/ui_simulator/run_ui_simulator_e2e.sh
```

ou sous Windows :

```powershell
.\tests\ui_simulator\run_ui_simulator_e2e.ps1
```

The first run downloads the pinned LVGL source into the ignored `.cache`
directory. To use an already available exact checkout:

```bash
./tests/ui_simulator/run_ui_simulator_e2e.sh \
    -LvglPath ./lv_port_pc_vscode/lvgl \
    -KeepArtifacts
```

```powershell
.\tests\ui_simulator\run_ui_simulator_e2e.ps1 `
    -LvglPath .\lv_port_pc_vscode\lvgl `
    -KeepArtifacts
```

After an intentional and visually reviewed UI change, regenerate the hash
manifest:

```bash
./tests/ui_simulator/run_ui_simulator_e2e.sh -UpdateBaselines -KeepArtifacts
```

```powershell
.\tests\ui_simulator\run_ui_simulator_e2e.ps1 -UpdateBaselines -KeepArtifacts
```

Screenshot approval is a PC rendering regression gate. It does not replace the
P4 DSI/PPA fluidity, touch-coordinate, visibility-at-distance or panel-timing
hardware acceptance.

## JC1060 gate (dj_ui)

`run_ui_simulator_e2e_jc1060.sh` builds the JC1060 presentation layer
(`firmware/main-deck-jc1060/components/ui/dj_ui.c`) with the deterministic
`dj_ui_demo.c` data source and runs `dj_ui_simulator_e2e.c` on a headless
1024x600 framebuffer. Navigation uses the real LVGL click callbacks; waveform
seek and screensaver wake go through a scripted pointer indev. Hashes are in
`baselines_jc1060.json`.

Deck 1 uses the built-in PEAKS surfaces, Deck 2 the IMAGE surfaces (RGB565
buffers standing in for the ANLZ wave cache), and `overview_external` covers the
EXTERNAL (direct PPA blit) contract. Besides the captures, the scenario asserts
surface geometry (`dj_ui_wave_get_area`, `dj_ui_wave_ms_to_x`), mini-waveform
seek, CDJ-style CUE, the library load gate, DJ Link peers/master, tappable
Settings fields, recording, screensaver wake and blackout.

Captures: `overview`, `overview_playing`, `overview_cue`, `overview_external`,
`library`, `library_busy`, `library_loaded`, `hot_cues`, `settings`,
`settings_link_off`, `overview_restored`, `screensaver`, `blackout`.

```bash
./tests/ui_simulator/run_ui_simulator_e2e_jc1060.sh -KeepArtifacts
./tests/ui_simulator/run_ui_simulator_e2e_jc1060.sh -UpdateBaselines -KeepArtifacts
```

After the captures, the scenario also drives the firmware bridge
(`ui_djui_bridge.c`, the only JC1060 presentation since v293) on a fresh
dj_ui tree and checks title, BPM, tempo, position and status. With
`UI_SIM_DEBUG=1` it prints the host cost of one zoom-surface refresh per
source (PEAKS / IMAGE / EXTERNAL); that is a ratio, not a P4 measurement.

This gate does not replace an ESP-IDF build or P4 PPA/touch hardware
acceptance.
