# Synthèse recherche web — glitchs audio USB + DSI + SDIO sur JC1060 (P4)

Date: 2026-09-20. Complète `docs/jc1060-audio-glitches.md` (investigation v23-v51).
Rapports sources (générés par recherche web) :
- `docs/research/cohabitation_dsi_usb_p4_recherche_web.md` (DSI + USB)
- `docs/research/usb_iso_out_glitches_dwc2_tinyusb.md` (USB iso OUT / DDJ)
- `docs/research/ESP32_P4_USB_ISO_JITTER_SOURCES.md` (ESP-Hosted / jitter P4)

## Rappels de l'état mesuré

Host prouvé propre : périodicité iso parfaite (0 missed deadline sur 30 s),
0 silence injecté (FIFO toujours alimentée), cadence exacte 44,1 kHz.
Glitchs présents AVEC et SANS Wi-Fi (v51) → facteur unique non isolé.

## Candidats classés par plausibilité

### 1. Contention PSRAM impliquant le fetch DSI (fort)
- DSI lit la PSRAM en continu (~74 MB/s pour 1024x600 RGB565@60). Toute
  pression additionnelle (SDIO Wi-Fi, PPA, cache miss) allonge les latences.
- **PSRAM gelée pendant TOUTE écriture flash** (ESP-FAQ officiel) : chaque
  log NVS, chaque write flash stoppe l'accès PSRAM → si le framebuffer DSI
  vit en PSRAM, le DSI underrun → le bus est pris en rafale → l'USB iso
  encaisse. Nos logs WARN passent par la console USB... mais NVS/OTA/eth
  writeback existent encore.
- Bug cache writeback P4 (esp-idf #18235, corrigé en fff1564, présent en
  v6.0.2) : preuve Espressif que cache + DMA USB + masters concurrents est
  un terrain connu et piégeux sur P4.

### 2. Mismatch ESP-Hosted C6 2.3.0 / host 2.12.0 (fort, hors support)
- Officiellement non supporté (esp-hosted-mcu #215) — comportement
  indéterminé (stalls RPC, timeouts). Toute conclusion impliquant le Wi-Fi
  reste douteuse tant que ce mismatch existe. Upgrade C6 impossible sur
  notre carte (pins absentes) → envisager downgrade host esp-hosted vers
  une version compatible 2.3.0, ou vivre sans Wi-Fi sur ce hardware.

### 3. Sensibilité du DAC adaptive du DDJ au pattern de transactions (moyen)
- DAC famille TI PCM29xx (adaptive, resynchro sur SOF) : sensible au
  jitter intra-microframe et au pattern, pas au débit moyen. Diagnostics :
  - période des glitchs FIXE → underrun device déterministe ;
    ALÉATOIRE → pertes PHY/bus.
- Comparer byte-exact avec la capture usbmon Linux (ddj*.pcap dans /tmp)
  qui est connue clean : nb transactions, tailles, espacement.

### 4. Buffers DMA USB hors SRAM interne (à vérifier — rapide)
- Doc Espressif : le DMA DWC2 n'accède QUE la SRAM interne. Les buffers
  TinyUSB (ep_buf, ff_buf de la FIFO stream) sont des tableaux statiques
  → .bss → SRAM interne : probablement OK, mais à vérifier (map file) que
  rien (scratch, WAV en flash OK) n'est en PSRAM.

### 5. Divers
- CONFIG_PM/DFS off (défaut IDF : off ✓ vérifier).
- FREERTOS_HZ=1000 ✓ déjà.
- `SPIRAM_XIP_FROM_PSRAM` : NON activé chez nous ✓ (casse esp-hosted,
  esp-idf#15997).

## Plan de tests priorisé (bissection propre)

1. **Figer le framebuffer** : DSI en refresh pur, aucun update LVGL
   (pas de log screen). Si propre → contention PSRAM/DSI confirmée →
   réduire la charge (résolution/refresh, framebuffer, log off pendant
   playback). Sinon étape 2.
2. **Revert le clamp microframe 88 B** (v49) — introduit APRÈS le dernier
   run propre (v37). Re-test A/B strict.
3. **Bissection SDIO** : v37 config + Wi-Fi OFF mais transport SDIO init
   quand même (ou l'inverse) pour séparer "présence du C6" de "trafic
   Wi-Fi".
4. **Downgrade esp-hosted host** vers une version supportant le co-proc
   2.3.0 (aligner les versions officiellement).
5. **Audit map file** : tous les buffers USB DMA en SRAM interne.
6. **Mesurer la période des glitchs** à l'oreille/capture : fixe vs
   aléatoire → oriente device underrun vs pertes bus.
7. **Répliquer le pattern usbmon Linux** (nb/taille/espacement des
   transactions) dans audioh_stream_prepare.

## Sources principales
- esp-idf #18235 (cache writeback P4, fix fff1564)
- LVGL #9590 (DSI underrun / PPA burst) + PR lvgl#9612
- ESP-FAQ LCD/PSRAM freeze during flash ops
- esp-hosted-mcu #215 (version mismatch), #167, #184, #144, #120, #210
- Docs Espressif : usb_host_notes_dwc_otg, sdmmc P4, power_management P4
- TinyUSB #1618 #2433 #2620 #3635 ; TI PCM290x datasheets ; usb_stream
  (Espressif) comme référence polling UAC1
- Voir les 3 rapports pour l'intégralité des URLs.
