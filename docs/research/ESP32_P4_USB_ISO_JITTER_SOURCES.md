# Recherche : jitter/glitchs audio USB isochrone sur ESP32-P4 — ESP-Hosted SDIO (C6) + autres sources

Date : 2026-09-20. Contexte : IDF 6.0.2, esp_hosted managed (host 2.12.0 vs C6 firmware 2.3.0),
SDIO 4-bit slot 1 GPIO 14-19, streaming mode. Glitchs USB iso présents Wi-Fi ON **et** OFF
(config v51) → recherche de toutes les sources de jitter, pas seulement esp-hosted.

## 1. ESP-Hosted SDIO (P4 host + C6) — ce qui est documenté

### 1.1 Charge CPU / tasks / IRQ
- Le driver SDIO de esp-hosted fait tourner des tâches de transport (process rx/tx) côté host
  plus des ISR SDMMC. En mode **streaming**, la polling/poll-task tourne en continu et sollicite
  le bus SDIO et le cache/CPU même sans trafic utile.
  - Doc officielle d'optimisation perf (référence pour les flags à vérifier) :
    https://github.com/espressif/esp-hosted-mcu/blob/main/docs/performance_optimization.md
  - Throughput réel P4+C6 SDIO bien en dessous du théorique, discussions de tuning :
    https://github.com/espressif/esp-hosted-mcu/issues/109
    https://www.reddit.com/r/embedded/comments/1kw1qrp/does_esphosted_offer_the_throughput_claimed_on/
- `esp_wifi_remote` ajoute une couche IPC + le stack Wi-Fi complet (TCP/IP sur host) :
  https://developer.espressif.com/blog/2025/09/esp-wifi-remote/

### 1.2 Stabilité P4+C6 SDIO — bug actifs connus (peuvent provoquer stalls/garbage bursts)
- **#167** — « Unrecoverable host SDIO state » : `sdmmc_host_wait_for_...` errors, crashes
  quelques secondes après boot. https://github.com/espressif/esp-hosted-mcu/issues/167
- **#184** — Inbound TCP stalls après ~100 KB (reproduction pure-IDF, Waveshare P4+C6) :
  https://github.com/espressif/esp-hosted-mcu/issues/184
- **#144** — asserts `sdio_rx_get_buffer` / `transport_drv_sta_tx` en download (réduire la
  mémoire heap SDIO est une mitigation mentionnée) : https://github.com/espressif/esp-hosted-mcu/issues/144
- **#120** — lockup « invalid ret or len_from_slave » après qq min de streaming :
  https://github.com/espressif/esp-hosted-mcu/issues/120
- **#210** — double-free `tlsf_free` en streaming, reproduit à 40 MHz **et 20 MHz** :
  https://github.com/espressif/esp-hosted-mcu/issues/210
- **#215** — version mismatch host/slave → `ESP_ERR_INVALID_ARG` ; le mismatch 2.3.0 (C6) vs
  2.12.x (host) est une source documentée de comportements imprévisibles :
  https://github.com/espressif/esp-hosted-mcu/issues/215

### 1.3 Interaction SDIO ↔ autres bus / PSRAM
- Le driver SDMMC peut copier via un buffer DMA temporaire si le buffer n'est pas DMA-capable :
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/storage/sdmmc.html
- `CONFIG_SPIRAM_XIP_FROM_PSRAM` casse esp-hosted (crashs, TLS pointers) — et plus largement
  l'exécution XIP depuis PSRAM augmente la pression cache :
  https://github.com/espressif/esp-idf/issues/15997
  https://github.com/espressif/esp-hosted-mcu/issues/36
- Mesures de jitter PSRAM sur P4 (thread « esp32 P4 performance », derrière bot-check, mais
  rapporte « Jitter / Lost datagrams » avec PSRAM 200 MHz) :
  https://esp32.com/viewtopic.php?t=41620
- Test fortement recommandé : passer esp-hosted en **Packet mode** (non-streaming) et/ou baisser
  la fréquence SDIO (issue #144: réduction heap SDIO ; #184/#167 pour les modes).

## 2. Causes génériques de jitter USB iso sur P4 (facteur confondant — glitchs aussi sans Wi-Fi)

### 2.1 Contrôleur USB DWC_OTG (host isochrone)
- Doc Espressif « USB Host Maintainers Notes (DWC_OTG) » : canaux périodiques (iso/interrupt)
  passent par le **PTX FIFO** séparé, scatter/gather DMA, scheduling périodique matériel :
  https://docs.espressif.com/projects/esp-usb/en/latest/esp32p4/usb_host/usb_host_notes_dwc_otg.html
  Tout ce qui retarde le re-remplissage des descripteurs DMA (task URB host, ISR, cache miss
  PSRAM) se traduit en missed microframes → glitchs iso.
- Throughput/rx flaky TinyUSB CDC sur P4 (signe de sensibilité du stack USB P4) :
  https://www.reddit.com/r/embedded/comments/1tx3yt8/low_esp32p4_tinyusb_usbcdc_throughput/
- Iso HS : limites de paquets par transfert : https://github.com/espressif/esp-usb/issues/279

### 2.2 Power management (CONFIG_PM / DFS / light-sleep)
- Doc officielle : PM ajoute de la latence d'interruption, **jusqu'à +40 µs** quand le scaling
  de fréquence est actif ; light-sleep gate les horloges périphériques :
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/system/power_management.html
- Mitigation : vérifier `CONFIG_PM_ENABLE=n` ou garder un lock `ESP_PM_CPU_FREQ_MAX`/`NO_LIGHT_SLEEP`
  pendant l'audio USB.

### 2.3 PSRAM / pression cache
- Code/audio buffers en PSRAM (HiRAM/XIP/cache-usage) → cache misses, contention AXI entre
  USB DMA, SDIO DMA, LCD/DPI, PSRAM. Doc external RAM :
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-guides/external-ram.html
- Mitigation : buffers ISO USB et anneau audio en SRAM interne ; réduire XIP-from-PSRAM ;
  vérifier `CONFIG_SPIRAM_SPEED` (200 vs 120 MHz).

### 2.4 FreeRTOS / priorités / tick
- Priorités : tâches USB host (TinyUSB/usb_host) et I2S/audio doivent être > prio LVGL/lwIP ;
  problème classique de stutter quand LVGL/affichage est trop prioritaire :
  https://www.reddit.com/r/esp32/comments/1s0ugrm/esp32_audio_playback_stuttering_when_drawing_to/
- Tick : `CONFIG_FREERTOS_HZ` (100 Hz par défaut → scheduling grossier) :
  https://github.com/adafruit/circuitpython/issues/9133
  (Arduino 1000 Hz corrigé des stutter audio : https://www.reddit.com/r/esp32/comments/16ekxxc/)
- Watchdogs/IRQ : https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/system/wdts.html

### 2.5 esp_timer et latence IRQ
- Doc esp_timer : callbacks dispatchés en interrupt = latence minimale ; sinon latence de
  réveil de tâche, sensible à la contention CPU :
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/system/esp_timer.html
- Latence d'interruption générale (disabling IRQ dans du code, cache) :
  https://esp32.com/viewtopic.php?t=38140
  https://haydendekker.medium.com/esp32-interrupts-can-only-do-200khz-56f8dbb6a61c

### 2.6 Compiler / fréquence CPU
- Doc perf générique Espressif (optimization -O2, XIP, caches) :
  https://docs.espressif.com/projects/esp-idf/en/v5.0/esp32s2/api-guides/performance/speed.html
- Vérifier `CONFIG_COMPILER_OPTIMIZATION` (-Og par défaut debug → jitter), `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`
  (P4 = 360/400 MHz attendu), et le pinning CPU (isoler audio/USB d'un core).

## 3. Plan d'actions suggéré (priorisé)
1. **Version slave** : même si l'upgrade est impossible, le mismatch 2.3.0/2.12.0 est officiellement
   non supporté (#215) — toute anomalie SDIO doit être testée avec les deux côtés alignés sur un
   banc de dev avant d'inculper esp-hosted en production.
2. Basculer esp-hosted **streaming → packet mode** et/ou SDIO 4-bit à fréquence réduite (20 MHz)
   en A/B pour isoler la contribution SDIO.
3. Vérifier `CONFIG_PM_ENABLE`, DFS, light-sleep (§2.2).
4. Buffers audio/USB ISO en SRAM interne, pas PSRAM (§2.3).
5. Priorités/pinning : USB host + audio > transport SDIO > LVGL ; `CONFIG_FREERTOS_HZ=1000`.
6. Mesurer : `esp_timer_get_time()` autour des callbacks URB iso, comptage des missed
   microframes, task WDT/`vTaskGetRunTimeStats` pour voir qui bouffe le CPU.
