# Cohabitation MIPI-DSI + USB Host sur ESP32-P4 — recherche web (2026-09-20)

Contexte : Pajoniiir, JC1060P470C_I_W (révision < v3), JD9165 1024x600 RGB565 @1000 Mbps,
USB host DWC2 HS (UTMI, TinyUSB vendored), audio isochrone OUT 4ch 44,1 kHz vers DDJ-400.
Glitchs audio périodiques, host prouvé propre côté logiciel (0 deadline miss).

---

## 1. Causes documentées, par ordre de pertinence

### A. Bug cache writeback P4 (TOUS les steppings) — résolu dans IDF v6.0.2 ✅
- Espressif a confirmé (issue #18235) : « In all P4 versions, there is an issue that under high
  memory load, some cache operations might fail. » Symptômes : DMA USB écrit/lu à de mauvaises
  adresses, corruption aléatoire, crashs après 129–400 s, proportionnels à la charge DMA.
- Fix : commit fff1564266ee1a9d18a38ef83d0aa8684e912e4c (MR !48498,
  `fix(esp_rom): avoid critical issue in writeback`, patch `esp_rom_cache_writeback_esp32p4_esp32s31.c`).
- **Vérifié : le fix est présent dans le tag `v6.0.2`** (components/esp_rom/esp32p4/esp_rom_caps.h
  contient `ESP_ROM_CACHE_WRITEBACK_NEEDS_SYNC_TWICE_MAP`) — donc déjà couvert, MAIS cela démontre
  que la classe de bugs « charge mémoire élevée + DMA USB + périphériques concurrents » est réelle
  sur P4 et que le rôle du cache L1/L2 dans les corruptions/glitches USB est confirmé par Espressif.
- Liens :
  - https://github.com/espressif/esp-idf/issues/18235
  - https://github.com/espressif/esp-idf/commit/fff1564266ee1a9d18a38ef83d0aa8684e912e4c

### B. Bug classe ESP32-S2 [USB-105] — arbitration AHB pendant DMA USB
- Errata S2 « Abnormal Data During AHB Bus Arbitration by USB OTG » : le DMA DWC2 corrompt les
  données lors de l'arbitrage AHB avec d'autres maîtres. Workaround S2 = hbstlen=INCR
  (appliqué dans usb_dwc_hal.c pour ECO0).
- Sur P4, USB et EMAC partagent le même bus AHB et le même port maître AXI (ICM port 0 « CPU »).
  Dans #18235, le workaround S2 (hbstlen 0/1/3/5) n'a PAS suffi — aucune valeur de burst ne
  corrige ; la cause finale était le cache (point A). Le rapport établit néanmoins que le
  DWC2 P4 est sensible aux masters DMA concurrents (EMAC, et par extension tout gros DMA
  comme le DSI/PPA/GDMA/SDIO) sur le silicon pre-v3.
- Lien : https://github.com/espressif/esp-idf/issues/18235 (analyse détaillée + tableau des workarounds testés)

### C. Underrun DSI = contention PSRAM (documentation Espressif officielle)
- Log Espressif : « can't fetch data from external memory fast enough, underrun happens » —
  l'arbitre PSRAM n'arrive pas à servir le fetch DPI framebuffer quand d'autres maîtres
  (CPU cache, PPA/GDMA, SDIO Wi-Fi) monopolisent la bande passante.
- Chiffres concrets (ddv2005, LVGL #9590) : 1280x720 RGB565 @60 Hz = le DSI lit la PSRAM à
  ~111 MB/s ; un PPA fill « lock » la PSRAM si longtemps qu'il fait underrun le DSI.
  Pour 1024x600 RGB565 @60 Hz le besoin DSI est ~74 MB/s — même ordre de grandeur.
- LVGL #9590 : réduire les burst lengths PPA de 128 à 64 bytes supprime les artefacts/underruns
  (fix mergé PR lvgl#9612, CONFIG_LV_PPA_BURST_LENGTH configurable).
- Consommateur caché dans Pajoniiir : **ESP-Hosted (Wi-Fi C6 sur SDIO)** — le SDIO est un maître
  DMA PSRAM supplémentaire dont les rafales peuvent coïncider avec le fetch DPI.
- Liens :
  - https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/lcd/mipi_dsi_lcd.html (section underrun)
  - https://github.com/lvgl/lvgl/issues/9590
  - https://github.com/lvgl/lvgl/pull/9612
  - https://lvgl.io/docs/open/integration/chip_vendors/espressif/tips_and_tricks

### D. La PSRAM est « coupée » pendant les écritures flash
- ESP-FAQ : « PSRAM is disabled during writes to flash. » Toute écriture flash (logger, NVS,
  OTA check, SPIFFS, stdout vers UART+flash op) gèle la PSRAM → l'arbitre DSI et le cache
  PSRAM sont suspendus. C'est un candidat CLASSIQUE pour des glitchs périodiques si un
  log/NVS se déclenche périodiquement pendant le streaming USB.
- Lien : https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/lcd.html

### E. Buffers USB/DMA en PSRAM = non supporté
- Le DMA DWC2 (Scatter/Gather, QTD lists) ne travaille qu'en mémoire interne (SRAM) ;
  les FIFOs sont copiées DMA <-> internal memory (doc officielle ESP-USB P4).
  Si un buffer de transfert TinyUSB finit en PSRAM (allocateur qui déborde), le comportement
  est indéfini. De même les descripteurs QTD doivent rester en SRAM interne.
- Lien : https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/usb_host/usb_host_notes_dwc_otg.html

### F. Interruptions / IRAM / cache-safe
- Recommandations Tasmota (P4 + MIPI-DSI, mesuré) : `CONFIG_LCD_DSI_ISR_IRAM_SAFE=y`,
  `CONFIG_LCD_DSI_ISR_CACHE_SAFE=y`, `CONFIG_GDMA_CTRL_FUNC_IN_IRAM=y`,
  `CONFIG_GDMA_ISR_IRAM_SAFE=y`, `CONFIG_COMPILER_OPTIMIZATION_PERF=y`,
  `CONFIG_CACHE_L2_CACHE_128KB` vs 256KB (arbitrage RAM interne vs cache).
- Un ISR USB qui n'est pas cache-safe + une miss de cache bloquée par un burst DSI/PPA
  = jitter d'interruption → glitch isochrone même sans deadline miss logiciel mesurable
  (le miss se produit au niveau ISR/matériel).
- Lien : https://github.com/arendst/Tasmota/discussions/24448
- Priorités d'interruption : la doc IDF (intr_alloc) reste la référence ; aucun bug DSI-vs-USB
  sur les niveaux d'IT n'a été trouvé publiquement — le problème documenté est la bande
  passante/cache, pas la priorité NVI.
- Lien : https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/system/intr_alloc.html

### G. errata / silicon pre-v3
- Les projets doivent cibler révision >= 3.1 dans les IDF récents — Espressif a durci le support
  car les differences pre-v3/v3 sont « huge hardware difference » (mot de tore-espressif dans #18235).
- Un document communautaire recense les errata P4 (miroir non officiel, à recouper avec la doc
  NDA Espressif) : https://github.com/solitary-dev-50/esp32-p4-design-resources/blob/main/docs/chip-revisions-and-errata.md
- Datasheet P4 (bande passante PSRAM : largeur 16 bits, max théorique = line_num x edge_mode x freq) :
  https://documentation.espressif.com/esp32-p4_datasheet_en.pdf

## 2. Ce qui n'a PAS été trouvé
- Aucun bug GitHub esp-idf documentant spécifiquement « DSI + DWC2 USB host audio glitch »
  comme paire connue. Les rapports connus sont USB+EMAC (#18235) et PPA/GDMA+DSI (LVGL #9590).
- Aucune issue publique sur un conflit de priorité d'interruption DSI vs USB.

## 3. Workarounds / actions concrètes pour Pajoniiir (triées)
1. Vérifier que tous les buffers TinyUSB (QTD, FIFO-adjacents, buffers isochrones) sont en SRAM
   interne (heap interne), jamais PSRAM (point E).
2. Auditer tout accès flash périodique pendant le streaming (logs, NVS commit, Wi-Fi calibration)
   → les déplacer hors des fenêtres audio ou les bufferiser (point D).
3. Mesurer la coïncidence glitchs / activité SDIO Wi-Fi : tester avec Wi-Fi désactivé, puis
   avec esp_hosted throttlé ; le SDIO est un maître PSRAM de plus (point C).
4. Si PPA/GDMA/LVGL accéléré est actif : réduire CONFIG_LV_PPA_BURST_LENGTH à 64 (point C).
5. sdkconfig : CONFIG_SPIRAM_SPEED_200M=y (si pas déjà), CONFIG_LCD_DSI_ISR_CACHE_SAFE=y,
   CONFIG_LCD_DSI_ISR_IRAM_SAFE=y, CONFIG_COMPILER_OPTIMIZATION_PERF=y, L2 256KB+line 128B
   (recommandation officielle Espressif anti-underrun, points C/F).
6. Garder en tête que sur silicon pre-v3, toute nouvelle charge DMA (ex. ajout Ethernet,
   caméra CSI) a un historique de révéler des corruptions USB — tester chaque ajout isolément.
7. Test de diagnostic décisif proposé par la communauté : désactiver l'update du framebuffer
   (image figée, DSI en refresh pur) → si les glitchs disparaissent, la cause est la contention
   PSRAM du fetch DPI ; si ils persistent, orienter vers USB/device-side.

## 4. Sources (toutes vérifiées 2026-09-20)
- Issue esp-idf #18235 (USB OTG DMA + EMAC, cache writeback, fix fff1564, présent en v6.0.2) :
  https://github.com/espressif/esp-idf/issues/18235
- Commit fix cache writeback : https://github.com/espressif/esp-idf/commit/fff1564266ee1a9d18a38ef83d0aa8684e912e4c
- LVGL #9590 (PPA -> DSI underrun, burst 64) : https://github.com/lvgl/lvgl/issues/9590
- LVGL PR #9612 : https://github.com/lvgl/lvgl/pull/9612
- esp-iot-solution MIPI DSI guide (underrun + frame rate opts) :
  https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/lcd/mipi_dsi_lcd.html
- ESP-USB P4 DWC_OTG notes (DMA en internal memory, FIFOs, QTD) :
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/usb_host/usb_host_notes_dwc_otg.html
- ESP-FAQ LCD (PSRAM off during flash writes) :
  https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/lcd.html
- LVGL/Espressif tips (SPIRAM 200M, PPA burst, alignement 64B) :
  https://lvgl.io/docs/open/integration/chip_vendors/espressif/tips_and_tricks
- Tasmota discussion 24448 (mesures P4 + DSI, ISR IRAM/cache-safe, L2 128K) :
  https://github.com/arendst/Tasmota/discussions/24448
- Datasheet ESP32-P4 : https://documentation.espressif.com/esp32-p4_datasheet_en.pdf
- esp32.com — CSI + USB HS coexistence (question throughput) : https://esp32.com/viewtopic.php?t=47668
- esp-hosted-mcu #109 (débit Wi-Fi SDIO sur P4) : https://github.com/espressif/esp-hosted-mcu/issues/109
- esp-idf #12996 (statut support P4, USB HS only) : https://github.com/espressif/esp-idf/issues/12996
- Errata P4 (miroir communautaire) :
  https://github.com/solitary-dev-50/esp32-p4-design-resources/blob/main/docs/chip-revisions-and-errata.md
