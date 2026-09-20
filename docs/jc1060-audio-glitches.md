# JC1060 — DDJ-400 USB audio glitches: investigation log

Status: **CAUSE ISOLÉE (v52-v54) — le flush DSI récurrent (LVGL) fait
glitcher l'USB iso.** Root cause du conflit bus en cours d'analyse
(recherche web dédiée). Branch `codex/ddj400-jc1060-integration`.

## Bissection décisive (2026-09-20, session 2)

| v   | config USB (clamp 88B) | écran/log            | résultat |
|-----|------------------------|----------------------|----------|
| 52  | reverté (paquet plein) | log figé (pas de refresh DSI) | **PROPRE** |
| 53  | reverté                | log fullscreen défilant | glitchs |
| 54  | reverté                | log bandeau 200px défilant | glitchs |

- Le clamp 88 B (v49) était **inocent** : revert sans effet.
- Le facteur qui fait apparaître/disparaître les glitchs = **le refresh DSI
  récurrent** (lv_label_set_text → invalidation LVGL → flush DMA framebuffer
  PSRAM). Pas la surface (bandeau 200 px suffit à glitcher) → c'est la
  PRÉSENCE de bursts DSI/PSRAM, pas leur taille totale.
- Le Wi-Fi était un **faux coupable** (v53 wifi OFF + log actif = glitchs
  quand même ; v36/v37 « propres » n'avaient pas de log défilant).

Modèle physique probable : chaque flush LVGL = gros memcpy/ DMA
SRAM↔PSRAM saturant le bus quelques ms → le canal périodique DWC2 n'est
pas ré-armé dans sa fenêtre SOF → paquet iso perdu SANS aucune trace
côté host (pas d'erreur, complétions suivantes stables — cohérent avec
l'instrumentation qui ne voit rien).

## FIX TROUVÉ (v57) : flush par 2D-DMA

`esp_lcd_dpi_panel_enable_dma2d(panel)` (chemin `use_dma2d=true` de la
démo fabricant / config standard ESPHome) remplace le memcpy CPU par un
copieur 2D-DMA → **son PROPRE avec log défilant ET Wi-Fi actif**.
Le Wi-Fi n'était jamais coupable : en v38 on avait réactivé Wi-Fi et le
log défilant (memcpy CPU) en même temps — confondant parfait.

### État couleurs (v57-v65)
- `rgb_ele_order = RGB` (démo fabricant + ESPHome + EspControl) : appliqué
  v57 — n'a PAS corrigé le magenta seul.
- `invert_color(true)` (v61) : compense l'inversion globale (texte devient
  cyan-vert = le 0x30d060 d'origine) mais fond devient clair → conso trop
  élevée, retiré en v64.
- Reste à faire : utiliser la table d'init du BON panel (SKU étiquette
  10153004-V2 = New_Panel) ; nos cmds actuelles = Old_Panel.
- Rapports recherche : docs/research/, docs-research-guition-jc1060p470-esphome.md

### État Wi-Fi (v58-v65)
- v58 : Wi-Fi réactivé → transport C6 muet/drop H_SDIO, scan bloquant
  figeait TOUT le boot (RPC C6 mort) → v62 : wifi_console_start 100 %
  asynchrone (plus de scan bloquant ni d'attente IP dans le boot).
- v63 : WIFI_PS_NONE retiré (héritage de la théorie invalidée).
- v65 : retry immédiat + watchdog 10 s.
- Reste : association échoue avec reasons 2 (AUTH_EXPIRE) / 203
  (ASSOC_FAIL) / 205 (CONNECTION_FAIL), puis transport muet. Le mismatch
  esp-hosted host 2.12.0 vs co-proc 2.3.0 (issue #215, officiellement non
  supporté) est le suspect principal : l'auth passe par des RPC.
  Options : (A) downgrade composant host esp_hosted vers la release
  alignée 2.3.0, ou (B) upgrade firmware C6 via UART header 2x10
  (CHIP_PU/IO9) vers l'image slave 2.12.x.


## Symptom

DDJ-400 (USB audio OUT, 4ch 44.1 kHz) plays audible audio from the P4 host
firmware but with periodic glitches (dropouts/clicks), regardless of S16 or
S24 format. Audio is clean when the DDJ is driven from a Linux PC (validated
earlier with usbmon captures and speaker-test).

## What was ruled OUT (measured, not guessed)

Instrumentation added in v48-v50 (`audio_host.c` vendored copy):

- `iso period` warning: logs any gap > 2.5 ms between playback transfer
  completions. Result across multiple 30 s captures with glitches present:
  **0 misses** (the only hit was a DDJ replug, 4.5 s).
- `SILENCE injected` counter: logs every interval where the stream FIFO ran
  short and the driver had to send silence. Result: **0 injections** — the
  FIFO was continuously fed.
- `tone: avail=0 wrote_total=...` (usb_tu_app.c): producer wrote 5.16 M
  frames ≈ full uptime at 44.1 kHz — the writer loop is healthy.

Conclusion: the host delivers a continuous, well-timed, correctly-cadenced
44.1 kHz stream. The glitch is NOT host scheduling, NOT FIFO underrun, NOT
Wi-Fi-induced deadline misses.

## What was tried (versions v23 → v51)

| v   | change                                                              | result |
|-----|---------------------------------------------------------------------|--------|
| 23  | esp_lvgl_port display, RGB565, log screen on tablet                 | display OK |
| -   | audio audible for the first time (FIFO depth fix)                   | but glitches |
| 32  | remove diagnostics from audio path, bigger tone scratch             | glitches |
| 33  | sine pre-encoded in final wire format in PSRAM (pure memcpy loop)   | glitches |
| 34  | tuh_task yield loop, prio 6, pinned cores                           | glitches |
| 35  | embedded WAV (flash, memory-mapped) instead of generated tone       | glitches (worse?) |
| 36  | isolation: display/touch/Wi-Fi/OTA/Eth all OFF                      | **CLEAN** |
| 37  | + display/LVGL back ON                                              | **CLEAN** (white screen, log off) |
| 38  | + Wi-Fi console + OTA back ON                                       | glitches |
| 39  | WIFI_PS_NONE                                                        | glitches (worse) |
| 40  | compiler PERF + log WARN (matching main-deck-p4 prod config)        | glitches |
| 41  | audio tasks prio 24 (> ESP-Hosted 23)                               | glitches |
| 42  | esp_wifi_stop() while streaming                                     | glitches |
| 43  | esp_hosted_deinit() while streaming                                 | glitches |
| 44  | reconnect-block while streaming (wifi_transition_lease pattern)     | glitches |
| 45  | SDIO clock 40→20 MHz (from deepwiki esp-hosted perf page)           | glitches |
| 46  | ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=n (SRAM transport buffers)         | system broken (screen off) — reverted |
| 47  | CMD53 DMA burst capped at 1024 B (sdio_drv.c vendored patch)        | glitches |
| 48  | iso-period instrumentation                                          | 0 misses |
| 49  | iso OUT microframe segmentation 88 B (kernel-like), hcd_dwc2.c      | glitches |
| 50  | silence-injection counter                                           | 0 injections |
| 51  | Wi-Fi OFF again (v37-equivalent)                                    | **STILL GLITCHES** |

## Important finding (v51)

v51 re-creates the v37 "clean" configuration (Wi-Fi/OTA/Eth OFF) **but the
glitches persist**. Therefore the earlier v36/v37 clean runs had a confounding
factor — the Wi-Fi was never proven to be the cause, and the currently active
USB path differences (microframe 88 B segmentation, SDIO 20 MHz, CMD53 burst
cap) are also suspects that were introduced *after* the clean runs. The
clean/dirty variable is **not yet isolated**.

## Key facts about the setup

- DDJ-400: VID/PID 2B73:0026, HS. Ifc1 alt1 = 4ch S16LE 44.1k (EP 0x01 OUT,
  MPS 576), alt2 = 4ch S24_3LE. No Feature Unit, no feedback EP → fixed
  cadence (44 frames / 1 ms / 352 B for S16).
- TinyUSB vendored (`components/tinyusb/tinyusb_src/`), P4 DWC2 HS 0x50000000,
  UTMI PHY + `tuh_configure(use_hs_phy=true)` + explicit `usb_utmi_hal_init()`.
- `CFG_TUH_AUDIO_STREAM_BUFSIZE` must be ≤ 32768 (tu_fifo hard limit
  `depth > 0x8000` fails silently → FIFO depth 0 → pure silence — the bug that
  cost us the whole "silence" phase).
- Driver keeps ONE iso transfer in flight (vs 6 URBs on Linux).
- DDJ-400 RE docs: github.com/palmarci/ddj400_re (SH7266 firmware).
- SD card: **no card in the slot** — sdmmc 0x107 / vfs_fat errors in logs are
  expected noise, ignore them.
- Screen: MIPI-DSI JD9165, RGB565, 1000 Mbps, esp_lvgl_port, log screen tee.

## Next leads (untested)

1. Re-isolate the clean/dirty variable properly: v51 differs from v37 by the
   88 B microframe clamp, SDIO 20 MHz, CMD53 burst cap, instrumentation.
   Bisect those one at a time against a clean v37 baseline.
2. Revert the 88 B microframe clamp (v49) and re-test — it was introduced
   after the last clean run.
3. DeepWiki/esp-hosted tuning items not yet applied: WIFI_RMT static/dynamic
   RX buffers, AMPDU, LWIP windows.
4. Device-side hypothesis: DDJ iso input FIFO behavior with a host that has
   no feedback EP and sends fixed-cadence packets; compare byte-exact
   transaction timing with the Linux usbmon capture (ddj*.pcap in /tmp).
5. Consider the monitor_pcm_link / I2S internal route as an interim path.

## Files touched during this investigation

- `main/usb_tu_app.c` — writer task, WAV embed, wifi suspend hooks (now
  unused), instrumentation counters.
- `main/wifi_console.c` — reconnect suppression hook, WIFI_PS_NONE.
- `main/main.c` — feature #if 0/1 toggles for isolation builds.
- `main/tone4ch.wav` — embedded test WAV (EMBED_FILES).
- `components/tinyusb/tinyusb_src/class/audio/audio_host.c` — vendored:
  diagnostics, silence counter, iso-period meter.
- `components/tinyusb/tinyusb_src/portable/synopsys/dwc2/hcd_dwc2.c` —
  vendored: 88 B iso OUT microframe clamp.
- `managed_components/espressif__esp_hosted/.../sdio_drv.c` — CMD53 burst cap
  1024 B (managed component — re-apply after `idf.py` re-resolve!).
- `sdkconfig.defaults` — PERF, WARN log, SDIO 20 MHz, PS_NONE-related notes.
