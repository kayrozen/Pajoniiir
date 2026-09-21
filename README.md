# Pajoniiir BL-A1800

Standalone dual-deck DJ system built around a Pioneer DDJ-FLX4, a Seeed
Studio XIAO ESP32S3 control board and a JC4880P443C_I_W ESP32-P4 multimedia
board. It reads Rekordbox media directly and does not require a PC during
performance.

Canonical repository: `https://github.com/dvucinozd/Pajoniiir.git`. The former
`dvucinozd/ESP32-DDJ-FLX4` URL is deprecated and retained only as a GitHub
redirect. The branch inventory was audited on 2026-07-26; only `master`
remains locally and on `origin`.

![Pajoniiir](docs/images/122.jpg)

> [!IMPORTANT]
> The ESP-IDF 6.0.2 migration is **merged into `master`**; both targets now
> build only under **ESP-IDF v6.0.2** (the component manifests pin
> `idf: "==6.0.2"`). The release prefix therefore moved from `RC1` to **`RC2`**,
> and the latest clean dual-target release build is **`RC2`** (`56905c89`) from
> 2026-07-30 — see
> [CLEAN_RELEASE_RC2_BUILD.md](docs/validation/CLEAN_RELEASE_RC2_BUILD.md). It
> was signed, packaged and installed successfully through OTA on both boards
> on 2026-08-02. It carries the full
> `fix/release-blockers-and-concurrency` stabilisation set (bounded compressed
> audio cache, paginated Library UI, immutable track sort, recorder safety
> hardening, lossless control queue, ANLZ ownership fixes and more).
>
> RC2 hardware acceptance is now **in progress**. Both targets have complete
> ESP-IDF v6.0.2 boot chains; the P4 microSD regression is fixed, and a focused
> 2026-08-02 smoke passed display/touch/Library, FLX4 MIDI/LED, MAIN/headphone
> audio and real-MP3 playback. Real WAV/FLAC cache testing was not performed:
> the Rekordbox database referenced files that were absent from the USB drive.
> Long-duration, USB recovery and fault-injection rows remain open in
> [ESP_IDF_6_0_2_MIGRATION.md](docs/migration/ESP_IDF_6_0_2_MIGRATION.md), so
> the latest **complete** functional hardware baseline remains
> **`RC1-123-g587cd7a1`** of 2026-07-14. See
> [Documentation Status](docs/DOCUMENTATION_STATUS.md) for the precise boundary.
>
> A later P4-only signed development update installed
> `RC2-51-g050ab43` into `ota_0` on 2026-08-22 and reached `valid`. COM15
> confirmed the 29,520 MB SDHC mount and, after one physical USB reinsert, an
> exFAT library load of 324 tracks. The already-inserted USB medium had first
> exhausted eight automatic enumeration-recovery cycles, so the reboot recovery
> row remains open. The S3 was not updated in that session and continued to
> report `RC2-44-g1923a3b`. See
> [RC2-51 P4 OTA deployment](docs/validation/RC2_51_P4_OTA_DEPLOYMENT_20260822.md).

## System at a Glance

| Device | Responsibility |
| --- | --- |
| **Pioneer DDJ-FLX4** | Operator surface: transport, jogs, tempo, mixer, pads, cue and LEDs |
| **XIAO ESP32S3** | USB MIDI host, semantic event translator, LED bridge, FLX4 USB-headphone streamer and service OTA AP |
| **ESP32-P4 board** | Authoritative playback/deck state, Rekordbox library, LVGL UI, audio DSP/mixer and MAIN/cue routing |

The S3 normalizes FLX4 input but does not own playback state. The P4 makes all
authoritative deck, mixer, audio-position and LED decisions. Both boards use
the existing `0xA5` UART control link, extended with the `0xA6` bulk/status
layer. The detailed ownership and data flow are documented in
[Architecture](docs/ARCHITECTURE.md).

## Current Capabilities

- Two independent decks with Rekordbox library browsing and MP3, WAV and FLAC
  playback. Compressed audio uses a bounded LRU page cache (8 × 32 KiB per
  deck) instead of whole-file PSRAM allocation. The current WAV subset is
  classic RIFF/WAVE PCM16 mono/stereo.
- FLX4 transport, jog/vinyl scratch, tempo and Master Tempo, mixer/EQ,
  headphone cue, hot cues, loops, beat jump/sync, Pad FX and Beat FX control.
  Beat FX Filter and Echo have recorded hardware acceptance; Flanger and the
  new one-shot Delay are software-tested and deployed, with focused physical
  audio/routing smoke still pending.
- Simultaneous PCM5102A RCA MAIN output and FLX4 USB headphone cue.
- P4-owned FLX4 LED feedback with reconnect and board-reboot resynchronization.
- LVGL Overview, Library (paginated 8-row table with PREV/NEXT), Hot Cues and
  Settings tabs, plus the optional P4 Wi-Fi remote.
- Data-driven controller profiles loaded from SD or installed through the web
  UI; the built-in DDJ-FLX4 map remains the fallback. The web overwrite path is
  software-complete and still has pending hardware-acceptance rows. A
  host-qualified Hercules DJControl Inpulse 500 profile is included; physical
  MIDI/LED/reconnect and USB-audio qualification remains pending.
- Signed dual-slot OTA, validation and rollback on both processors.

## Upcoming Targets

The following integrations are **work in progress** (uncommitted):

- **Pioneer DDJ-400**: MIDI-CI parser with full mapping for transport, 6 hot
  cues (A-F), jog, pitch, EQ/filter, manual loop, beat jump, 13 Pad FX and
  Beat FX. Differs from the FLX4 in having 6 hot cues instead of 8, no LCD, no
  Smart CFX and dedicated physical loop buttons. See
  `controllers/pioneer_ddj_400/`.
- **Guition JC1060P470C 7" display**: new ESP32-P4 target with JD9165
  MIPI-DSI 1024x600 panel, GT911 capacitive touch and ES8311 audio codec.
  Hardware bring-up COMPLETE on hardware (v117, ESP-IDF 6.0.2): display
  colours (v69 INVOFF fix), clean audio (v57 DMA2D fix), USB host DDJ via
  TinyUSB, and Ethernet (RMII + IP101GR, DHCP working, TCP debug console
  :2333). Wi-Fi (ESP32-C6 via esp_hosted) parked under IDF 6.0.2 - reference
  plan on IDF 5.5.5. LVGL UI porting is in progress. See
  [`firmware/main-deck-jc1060/BRING_UP_GUIDE.md`](firmware/main-deck-jc1060/BRING_UP_GUIDE.md)
  and [`docs/recherche/eth-jc1060p470-phy-is-busy.md`](docs/recherche/eth-jc1060p470-phy-is-busy.md).

Detailed implementation and acceptance status belongs in
[Project Overview](docs/PROJECT_OVERVIEW.md),
[Development Plan](docs/DEVELOPMENT_PLAN.md) and
[Documentation Status](docs/DOCUMENTATION_STATUS.md), rather than in this
repository entry page.

## Interface

The captures are representative; small UI details may be newer in firmware.

| Overview | Library | Settings |
| --- | --- | --- |
| ![Overview screen](docs/images/overview.jpg) | ![Library screen](docs/images/library.jpg) | ![Settings screen](docs/images/settings.jpg) |

The Hot Cues tab is implemented but does not yet have an archived screenshot.

## Repository Layout

```text
controllers/                 Compiled and source controller profiles
  pioneer_ddj_flx4/          DDJ-FLX4 profile (production, hardware-verified)
  pioneer_ddj_400/           DDJ-400 MIDI-CI parser and mapping research (WIP)
  hercules_djcontrol_inpulse_500/  Hercules Inpulse 500 profile (host-qualified)
  generic_midi_ci/           Generic profile path for host testing
firmware/
  control-board-s3/          ESP32-S3 host/translator/audio-bridge firmware
  main-deck-p4/              ESP32-P4 playback/audio/UI firmware (JC4880 4.3")
  main-deck-jc1060/          ESP32-P4 target for JC1060P470 7" 1024x600 (WIP)
  common/                    Shared firmware components
docs/                        Product, protocol, validation and design records
tests/                       PC-side regression tests
tools/                       Profile compiler, OTA packager and support tools
```

## Build and Test

Required baseline: **ESP-IDF v6.0.2** and its matching Espressif Python and
toolchain environment. Host tests additionally require native GCC/Make and
PowerShell 5.1 (ili noviji) na Windowsima, odnosno standardni shell na Linuxu.

A standard ESP-IDF installation can be initialized on Windows with:

```powershell
$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v6.0.2"
. "$env:IDF_PATH\export.ps1"
```

Verify the selected environment before configuring either target:

```powershell
idf.py --version
```

It must report `ESP-IDF v6.0.2`. For the first build after switching from IDF
5.5.4, remove the previous generated configuration and managed components:

```powershell
Remove-Item -Recurse -Force build, managed_components -ErrorAction SilentlyContinue
Remove-Item sdkconfig, sdkconfig.old -ErrorAction SilentlyContinue
```

Build each target from the repository root:

```powershell
cd firmware\control-board-s3
idf.py set-target esp32s3
idf.py build

cd ..\main-deck-p4
idf.py set-target esp32p4
idf.py build
```

Run the host regression suites from the repository root. These are the same two
entry points CI uses, and both run under Windows PowerShell 5.1 and PowerShell 7:

```powershell
.\tests\run_s3_host_tests.ps1
.\tests\run_p4_host_tests.ps1
```

If `gcc` is not already on `PATH`, append msys2 rather than prepending it —
prepending shadows the system `python.exe` with msys2's, which cannot run the
OTA signing suite:

```powershell
$env:Path = "$env:Path;C:\msys64\ucrt64\bin"
```

Run the headless LVGL navigation and exact-framebuffer screenshot gate:

```powershell
.\tests\ui_simulator\run_ui_simulator_e2e.ps1
```

The first run fetches the pinned LVGL source into the ignored `.cache`
directory. The gate covers Overview Deck 1/2 selection, Library, Hot Cues,
Settings, the screensaver and exact Settings restoration. See
[`tests/ui_simulator/README.md`](tests/ui_simulator/README.md) for baseline
review and update instructions. This PC gate does not replace P4 display,
touch or waveform-motion hardware acceptance.

Both default firmware configurations include the FLX4 USB-headphone path.
Build, flashing, signed release packaging and rollback procedures are covered
by [OTA Update](docs/OTA-UPDATE.md). Hardware bring-up and recurring acceptance
checks are in the [Startup Checklist](docs/STARTUP_CHECKLIST.md).

## Documentation

Start with the [complete documentation index](docs/README.md). The primary
operational documents are:

| Topic | Document |
| --- | --- |
| Product status and source-of-truth policy | [Documentation Status](docs/DOCUMENTATION_STATUS.md) |
| Product shape and implemented scope | [Project Overview](docs/PROJECT_OVERVIEW.md) |
| P4/S3 responsibilities and data flow | [Architecture](docs/ARCHITECTURE.md) |
| FLX4 inputs, outputs and acceptance ledger | [DDJ-FLX4 MIDI Map](docs/DDJ_FLX4_MIDI_MAP.md) |
| UART events and bulk/status transport | [Control Link Protocol](docs/CONTROL_LINK_PROTOCOL.md) |
| Wiring, USB and audio connections | [Hardware Wiring](docs/HARDWARE_WIRING.md) |
| Current phases and remaining engineering work | [Development Plan](docs/DEVELOPMENT_PLAN.md) |
| Deferred native folder/APTA library integration | [libapta P4 Integration Plan](docs/LIBAPTA_P4_INTEGRATION_PLAN.md) |
| Open and accepted risks | [Risk Register](docs/RISK_REGISTER.md) |

Controller-profile schema/update guides, OTA records, validation evidence,
historical design decisions and upstream/vendor references are linked from the
documentation index. Dated design records explain intent; they do not override
current firmware or active operational documents.
