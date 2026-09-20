# Recherche : ESPHome / EspControl et Guition JC1060P470 (ESP32-P4, JD9165, MIPI-DSI 1024x600)

Date : 2026-09-20. Sources vérifiées en ligne (docs officielles, code source ESPHome `dev`, repo espcontrol, issues GitHub).

## 1. ESPHome support natif ESP32-P4 + MIPI-DSI

- Composant officiel : `display: platform: mipi_dsi` (ESP32-P4 uniquement).
  Doc : https://esphome.io/components/display/mipi_dsi/
  Code : https://github.com/esphome/esphome/tree/dev/esphome/components/mipi_dsi
- **`JC1060P470` (Guition) est un modèle intégré officiellement** — pas besoin d'init custom pour la révision "Old_Panel". Définition verbatim dans
  `esphome/components/mipi_dsi/models/guition.py` :
  ```python
  DsiDriverChip(
      "JC1060P470",
      width=1024,
      height=600,
      hsync_back_porch=160,
      hsync_pulse_width=40,
      hsync_front_porch=160,
      vsync_back_porch=23,
      vsync_pulse_width=10,
      vsync_front_porch=12,
      pclk_frequency="54MHz",
      lane_bit_rate="750Mbps",
      color_order="RGB",
      initsequence=[
          (0x30, 0x00), (0xF7, 0x49, 0x61, 0x02, 0x00), (0x30, 0x01), (0x04, 0x0C), (0x05, 0x00), (0x06, 0x00),
          (0x0B, 0x11), (0x17, 0x00), (0x20, 0x04), (0x1F, 0x05), (0x23, 0x00), (0x25, 0x19), (0x28, 0x18), (0x29, 0x04), (0x2A, 0x01),
          (0x2B, 0x04), (0x2C, 0x01), (0x30, 0x02), (0x01, 0x22), (0x03, 0x12), (0x04, 0x00), (0x05, 0x64), (0x0A, 0x08),
          (0x0B, 0x0A, 0x1A, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x08, 0x1F, 0x1D),
          (0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
          (0x0D, 0x16, 0x1B, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x09, 0x1E, 0x1C),
          (0x0E, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
          (0x0F, 0x16, 0x1B, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1C, 0x1E, 0x09, 0x07),
          (0x10, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
          (0x11, 0x0A, 0x1A, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1D, 0x1F, 0x08, 0x06),
          (0x12, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
          (0x14, 0x00, 0x00, 0x11, 0x11), (0x18, 0x99), (0x30, 0x06),
          (0x12, 0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29,),
          (0x13, 0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29,),
          (0x30, 0x0A), (0x02, 0x4F), (0x0B, 0x40), (0x12, 0x3E), (0x13, 0x78), (0x30, 0x0D), (0x0D, 0x04),
          (0x10, 0x0C), (0x11, 0x0C), (0x12, 0x0C), (0x13, 0x0C), (0x30, 0x00),
      ],
  )
  ```
  → Même style de séquence page-select (`0x30` bank switching) que le kit Guition ; les valeurs diffèrent de la variante "New_Panel" du kit (voir §3). Les commandes 0x2A/0x2B ici sont des commandes vendor internes JD9165 (dans les banks), **pas** les commandes standard DCS 0x2A/0x2B (COLUMN/ROW address) qui ne servent qu'en mode DBI ; en mode DPI le framing est géré par le timing hardware.
- Aussi intégrés : `JC4880P443` (ST7701, 480x800, 34MHz pclk, 500Mbps — la 4.3" du kit Guition) et `JC8012P4A1` / `JC8012P4A1-V2` (JD9365).
- Options clés du composant (doc officielle) :
  - `color_order`: `bgr` (défaut générique) ou `rgb` — "ordre des canaux couleur du panneau. N'affecte pas l'ordre du buffer, qui est toujours RGB."
  - `invert_colors`: true/false (défaut false ; ESPHome envoie `INVOFF`/`INVON`).
  - `pixel_mode`: `16bit` (défaut) / `24bit` ; `color_depth`: 16 (défaut) / 24.
  - `byte_order`: `big_endian` / `little_endian` (défaut) — ordre des octets du buffer 16 bit.
  - `init_sequence`: liste de commandes supplémentaires envoyées **après** celles du modèle.
  - Dépendances obligatoires : `psram` + `esp_ldo` (canal 3, 2.5V pour la PHY DSI).
  - Source Python : `color_order` défaut = `MODE_BGR` mais les modèles Guition surchargent tous à `RGB`.

## 2. Comment la couleur est appliquée (mécanisme exact)

- `mipi_dsi.cpp` : buffer interne toujours RGB565 little-endian (ou RGB888 selon `color_depth`) ; `color_order` (BGR/RGB) ne change PAS l'ordre des octets du buffer. Ce qui configure le panneau, c'est **MADCTL (0x36)** et **PIXFMT (0x3A)** : le composant `mipi` ajoute automatiquement à la fin de toute init : `PIXFMT/MADCTL/INVOFF/SLPOUT/DISPON` (commentaire verbatim du profil espcontrol V2 : *"ESPHome appends PIXFMT/MADCTL/INVOFF/SLPOUT/DISPON, so do not add 0x3A, 0x36 or 0x29 here"*).
- Donc : `color_order: rgb` ⇒ bit BGR de MADCTL = 0 ⇒ le panneau interprète les pixels comme R,G,B. `color_order: bgr` (défaut) ⇒ bit BGR = 1 ⇒ R et B échangés à l'affichage.
- **Un vert affiché magenta = échange R↔B = problème MADCTL BGR bit, pas de byte-swap.** (Le byte-swap 16-bit donne plutôt un scrambling multicolore, cf. issue #16671.)
- Attention en 16-bit : avec LVGL, il faut `byte_order: little_endian` (défaut) — la doc officielle a été mise à jour suite à l'issue https://github.com/esphome/esphome/issues/16671 ("DSI displays need `byte_order: little_endian` in the LVGL config" — clydebarrow).

## 3. EspControl (jtenniswood) — les deux variantes de panneau JC1060P470

- **EspControl** = firmware ESPHome "no-code smart home control panel" de jtenniswood, flashable depuis le navigateur (web installer), repo : https://github.com/jtenniswood/espcontrol, docs : https://jtenniswood.github.io/espcontrol/. C'est une config ESPHome + composants externes (`espcontrol`, `web_server_idf`, `artwork_image`), pas un framework indépendant.
- Il maintient **deux profils séparés** exactement pour le même dualisme New/Old panel du kit :
  - `devices/guition-esp32-p4-jc1060p470/` (V1, "original panel", pas de marquage V2, date code < 2622)
  - `devices/guition-esp32-p4-jc1060p470-v2/` (2026 "New Panel", "V2" imprimé dans le SKU/matériel au dos)
  - Citation verbatim (V2) : *"Panels without that 'V2' marking need the original guition-esp32-p4-jc1060p470 profile; the wrong profile shows a white screen with a vertical noise band. Everything except the MIPI-DSI init sequence and timings, and the ESP32-C6 SDIO clock (10 MHz), matches that profile."*
- **Bloc display V1** (utilise le modèle intégré ESPHome) :
  ```yaml
  display:
    - platform: mipi_dsi
      id: my_display
      model: JC1060P470
      color_order: RGB
      reset_pin:
        number: GPIO05
      update_interval: never
      auto_clear_enabled: false
  ```
- **Bloc display V2 "New Panel"** (model CUSTOM + init JD9165 New_Panel tirée du kit Guition `JC1060P470C_I_W_Y.zip`, `esp_lcd_jd9165.c`) — verbatim :
  ```yaml
  display:
    - platform: mipi_dsi
      id: my_display
      model: CUSTOM
      color_order: RGB
      # Panel LCD_RST is GPIO0 on the JC1060P470C_I_W_Y schematic (FPC1 pin 4);
      # GPIO5 only reaches the expansion header. A wrong reset pin also suppresses
      # ESPHome's software reset, so a warm reboot (flash, OTA, restart) would keep
      # the previous firmware's panel registers until the next power cycle.
      reset_pin:
        number: GPIO0
      # an un-initialised JD9165 shows a bright lavender field, so the backlight
      # must not come on before init.
      setup_priority: 850
      update_interval: never
      auto_clear_enabled: false
      dimensions:
        width: 1024
        height: 600
      pixel_mode: 16bit
      color_depth: 16
      lanes: 2
      lane_bit_rate: 750Mbps
      pclk_frequency: 52MHz
      hsync_pulse_width: 24
      hsync_back_porch: 136
      hsync_front_porch: 160
      vsync_pulse_width: 2
      vsync_back_porch: 21
      vsync_front_porch: 12
      # JD9165 vendor init for the 2026 "New Panel" (Guition rear label "V2").
      # ESPHome appends PIXFMT/MADCTL/INVOFF/SLPOUT/DISPON, so do not add 0x3A,
      # 0x36 or 0x29 here. The 0x11 at the end of the table IS deliberate.
      init_sequence:
        # Load-bearing: the JD9165 needs 120 ms after reset before it accepts the
        # vendor table. Without it the panel comes up white on a warm reboot.
        - delay 120ms
        - [0x30, 0x00]
        - [0xF7, 0x49, 0x61, 0x02, 0x00]
        - [0x30, 0x01]
        - [0x04, 0x0C]
        - [0x05, 0x08]
        - [0x0B, 0x11]
        - [0x20, 0x04]
        - [0x1F, 0x05]
        - [0x23, 0x38]
        - [0x28, 0x18]
        - [0x29, 0x29]
        - [0x2A, 0x01]
        - [0x2B, 0x29]
        - [0x2C, 0x01]
        - [0x30, 0x02]
        - [0x00, 0x05]
        - [0x01, 0x22]
        - [0x02, 0x08]
        - [0x03, 0x12]
        - [0x04, 0x16]
        - [0x05, 0x64]
        - [0x06, 0x00]
        - [0x07, 0x00]
        - [0x08, 0x78]
        - [0x09, 0x00]
        - [0x0A, 0x04]
        - [0x0B, 0x16, 0x17, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x07, 0x09]
        - [0x0C, 0x09, 0x1E, 0x1E, 0x1C, 0x1C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D]
        - [0x0D, 0x0A, 0x05, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x06, 0x08]
        - [0x0E, 0x08, 0x1F, 0x1F, 0x1D, 0x1D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D]
        - [0x0F, 0x0A, 0x05, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1D, 0x1D, 0x1F]
        - [0x10, 0x1F, 0x08, 0x08, 0x06, 0x06, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D]
        - [0x11, 0x16, 0x17, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1C, 0x1C, 0x1E]
        - [0x12, 0x1E, 0x09, 0x09, 0x07, 0x07, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D]
        - [0x13, 0x00, 0x00, 0x00, 0x00]
        - [0x14, 0x00, 0x00, 0x41, 0x41]
        - [0x15, 0x00, 0x00, 0x00, 0x00]
        - [0x17, 0x00]
        - [0x18, 0x85]
        - [0x19, 0x06, 0x09]
        - [0x1A, 0x05, 0x08]
        - [0x1B, 0x0A, 0x04]
        - [0x26, 0x00]
        - [0x27, 0x00]
        - [0x30, 0x06]
        - [0x12, 0x3F, 0x25, 0x27, 0x35, 0x1D, 0x1B, 0x1B, 0x1A, 0x18, 0x0A, 0x2A, 0x21, 0x19, 0x30]
        - [0x13, 0x3F, 0x26, 0x27, 0x35, 0x1E, 0x1C, 0x1C, 0x1A, 0x18, 0x0B, 0x2A, 0x21, 0x19, 0x30]
        - [0x30, 0x0A]
        - [0x02, 0x4F]
        - [0x0B, 0x40]
        - [0x30, 0x0D]
        - [0x0D, 0x04]
        - [0x10, 0x05]
        - [0x11, 0x0C]
        - [0x12, 0x05]
        - [0x13, 0x0C]
        - [0x30, 0x00]
        # Deliberate SLPOUT, kept for vendor-timing fidelity: Guition's driver waits
        # 120 ms between sleep-out and display-on.
        - [0x11]
        - delay 120ms
  ```
- Contexte plateforme commun (les deux profils) :
  ```yaml
  esp32:
    variant: esp32p4
    flash_size: 16MB
    cpu_frequency: 360MHZ
    engineering_sample: true
    framework:
      type: esp-idf
      advanced:
        enable_idf_experimental_features: true
        execute_from_psram: true

  psram:
    mode: hex
    speed: 200MHz

  esp_ldo:            # (V2 ; V1 a aussi le canal 3 pour la PHY DSI)
    - channel: 3
      id: dsi_phy_enable
      voltage: 2.5V
      adjustable: True

  lvgl:
    id: main_lvgl
    displays: my_display
    rotation: 180
    byte_order: little_endian
  ```
  Touch GT911 : I2C `sda: GPIO07, scl: GPIO08`, reset GPIO22, interrupt GPIO21. Backlight PWM LEDC GPIO23.

## 4. Issue esphome/esphome#16873 (perf LVGL/MIPI-DSI P4)

URL : https://github.com/esphome/esphome/issues/16873 — "ESP32-P4 LVGL/MIPI DSI performance tracking: making 800x800 RGB888 UI usable" (ouverte par kyvaith, juin 2026, encore **ouverte** au 20/08/2026).

PRs liés :
- `[mipi_dsi] Add DMA2D-backed async flush support #16853`
- `[lvgl]` buffer fractions plus fines #16860, defines 9.5 #16861, disable runtime asserts #16862, **PPA draw unit #16863** (flags `use_ppa`, `use_ppa_img`), source filtering #16864, Lottie/SVG #16865, display staging #16866, diagnostics #16867 (`fps_benchmark`, `perf_monitor`, `profiler`), docs esphome.io#6747/6748.

Table de benchmarks verbatim (800x800 RGB888, PPA on, 48MHz pclk, 1.5Gbps, scroll dense) :

| Variant | Visual result | LVGL loop total | Flush total | Avg flush | Display copy | Notes |
| --- | --: | --: | --: | --: | --: | --- |
| Full render baseline | OK | 1.659 s | 362.7 ms | 15.1 ms | n/a | Heavy full-frame cost |
| Direct baseline | OK | 1.612 s | 268.7 ms | 11.2 ms | n/a | Better, still slow |
| Async DSI staging | OK | 1.811 s | 449.7 ms | 551 us | ~900 ms | Safe but expensive copy path |
| Unsafe zero-copy | Corrupted | 1.147 s | 31.0 ms | 37 us | 0 ms | Fast, but horizontal artifacts |
| Zero-copy + C2M cache sync | OK | 1.230 s | 42.2 ms | 51 us | 0 ms | Current best path |

Points d'architecture :
- Flush asynchrone **opt-in** : LVGL remet la frame au driver DSI et reçoit la complétion via `lv_display_flush_ready()` ; tâche FreeRTOS `mipi_flush_ready` pinée sur le core 0 ; l'ISR ne fait que signaler.
- Zero-copy seulement si buffer en RAM interne + adresses/stride alignés + **sync cache C2M avant la lecture DSI** (le zero-copy sans sync = corruption horizontale).
- Statut (20 août 2026) : en attente de décision mainteneurs ; docs PRs stale-closed ; LVGL `next` figé mi-juin. **Pas encore mergé dans le main** — le `mipi_dsi.cpp` actuel de `dev` fait un flush bloquant (sémaphore `io_lock_` sur `on_color_trans_done`) avec DMA2D activé (`esp_lcd_dpi_panel_enable_dma2d` sur IDF ≥ 6.0 ; `flags.use_dma2d = true` avant).

## 5. Configs communautaires / repos

- `jtenniswood/esphome-lvgl` → `guition-esp32-p4-jc1060p470/` : config ESPHome+LVGL minimaliste par package (pointe vers `package.yaml` du même repo). URL : https://github.com/jtenniswood/esphome-lvgl/blob/main/guition-esp32-p4-jc1060p470/esphome.yaml
- `charrus/Guition-esp32-p4-jc1060p470-Sci-fi-dashboard` : dashboard ESPHome pour cette tablette.
- `jtenniswood.github.io/esphome-media-player/devices/esp32-p4-jc8012p4a1.html` : sibling 10.1".
- Thread HA community "Guition esp32-p4-jc1060p470" : https://community.home-assistant.io/t/guition-esp32-p4-jc1060p470/959144 (pages 2-3 : problèmes d'init/écran blanc selon révision).
- atomic14 (fiche board) : https://www.atomic14.com/esp32/boards/guition-jc1060p470/ — "ESPHome: In progress" ; recommande ESP-IDF+LVGL sinon.
- openHASP issue #873 : demande de support JC1060P470 (JD9165 + GT911) confirmée.
- Board adaptation détaillée (pin map + init JD9165 depuis le demo Guition) : https://ai-box.eu/en/news/ein-neues-board-zu-esp-claw-hinzufuegen-meine-board-adaption-fuer-das-guition-jc1060p470/2195/ — pin map : I2C SDA=7/SCL=8, BL=23 (LEDC 20kHz), SDMMC CLK=43/CMD=44/D0-D3=39-42, C6 SDIO CLK=18/CMD=19/D0-D3=14-17/RST=54, I2S audio ES8311 MCLK=13/BCLK=12/WS=10/DOUT=9/DIN=48, PA_EN=11.

## 6. Synthèse couleur (réponse à la question du projet)

1. **Vert affiché magenta = bit BGR de MADCTL inversé.** Fix ESPHome : `color_order: RGB` sur le modèle (c'est ce que fait le modèle intégré `JC1060P470` ET les deux profils espcontrol). Le défaut générique du composant est `bgr`, d'où l'erreur classique. En firmware ESP-IDF maison : envoyer `MADCTL (0x36)` avec bit BGR (bit 3) = 0, ou mettre à 1 si le driver envoie déjà BGR=0 — bref inverser la valeur actuelle du bit 3.
2. **Ne pas toucher au byte-swap 16-bit pour ce symptôme** : byte-swap = scrambling psychédélique, pas R↔B (issue #16671). Avec LVGL laisser `byte_order: little_endian`.
3. **COLMOD 0x3A** : ESPHome l'ajoute automatiquement (PIXFMT) — 0x55 (16bit) ou 0x77/0x66 selon pixel_mode 16/24bit. En IDF maison : 0x3A=0x55 pour RGB565.
4. Les commandes vendor **0x51/0x55 ne figurent dans aucune des deux inits JD9165** trouvées (ni kit Guition New_Panel ni modèle ESPHome) — pas de preuve qu'elles inversent R/B sur ce panneau. L'échange R/B vient du MADCTL.
5. Les deux variantes d'init du kit (New_Panel/Old_Panel) correspondent exactement : modèle intégré ESPHome `JC1060P470` = Old_Panel ; New_Panel = `model: CUSTOM` avec la séquence du §3. Mauvaise variante = écran blanc avec bande de bruit verticale (pas un problème de couleur).
6. Reset panel : **GPIO0** (FPC1 pin 4) sur le schematic JC1060P470C_I_W_Y, pas GPIO5 (réservé header expansion). Obligatoire pour que le reset logiciel fonctionne sur warm reboot.

## Conclusion — ESPHome règle-t-il couleur + flush DSI ?

- **Couleur : OUI.** `model: JC1060P470` (ou CUSTOM + init New_Panel) avec `color_order: RGB` règle l'échange R/B. Chemin éprouvé par espcontrol en production sur les deux révisions.
- **Flush DSI : PARTIELLEMENT.** Le path standard ESPHome (flush bloquant + DMA2D sur IDF 6.x) fonctionne ; le flush asynchrone zero-copy (~10x plus rapide, 51us vs 551us) est dans les PRs de l'issue #16873 mais **pas encore mergé** (en attente mainteneurs au 20/08/2026). Pour un firmware audio temps réel (Pajoniiir), le point utile est l'architecture : flush DSI dans une tâche dédiée core 0, LVGL reste dans sa tâche, sync cache C2M obligatoire avant lecture DMA — reproductible en ESP-IDF pur sans attendre ESPHome.
