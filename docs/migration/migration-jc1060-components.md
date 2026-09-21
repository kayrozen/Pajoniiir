# Migration main-deck-p4 → main-deck-jc1060 : composants prêts

*Préparé le 2026-09-20. Basé sur l'audit des CMakeLists/idf_component.yml des
deux projets + l'état hardware actuel du JC1060P470C.*

## État cible (main-deck-jc1060, v96)

Fonctionnel aujourd'hui : écran (couleurs fixées v69), audio DDJ USB propre
(v57 DMA2D), console TCP:2333 via ETH (v89+, ETH bring-up v95, test croisé
"vendor example" : Link Up + Got IP validé sur IDF 5.5.4 ET IDF 6.0.2).
Parké : Wi-Fi/C6 (RPC hang IDF 6.0.2, dossier `docs/recherche/`), USB DDJ vs
USB MSC (conflit potentiel, voir ci-dessous).

## Niveau 1 — Fondations (aucune dépendance board, migrer tels quels)

| Composant | Sources | Notes |
|---|---|---|
| `app_settings` | 1 | NVS pur. Relire les clés (`wifi_remote` etc.) à l'occasion. |
| `sd_io_gate` | 1 | Verrou FS partagé. Indépendant du bus SD réel. |
| `media_io_gate` | 1 | Idem, couche au-dessus. |
| `hot_cue_store` | 1 | NVS pur. |
| `fatfs` | 13 | Wrapper FATFS. |
| `service_log` | 2 | Dépend `sd_io_gate` (niveau 1 ✓). Journal persistant. |

Communs déjà partageables via `EXTRA_COMPONENT_DIRS ../common/*` (comme
main-deck-p4) : `firmware_health`, `ota_manifest`, `control_state_reconciler`.

## Niveau 2 — Bibliothèque média (USB)

| Composant | Sources | Notes |
|---|---|---|
| `library` | 5 | PDB/ANLZ parser + index. Dépend media_io_gate/sd_io_gate ✓. |
| `media_catalog` | 1 | Au-dessus de library. |
| `usb_storage` | 6 | ⚠️ **Conflit USB à résoudre** : utilise `usb_host_msc`
(esp_usb) alors que le DDJ passe par TinyUSB host — les deux stacks veulent le
contrôle du DWC3. Options : MSC via TinyUSB, ou deux ports USB distincts si le
JC1060P470C en a, ou un arbitrage. À trancher AVANT la migration. |

## Niveau 3 — Audio (adaptation moyenne)

| Composant | Sources | Adaptation requise |
|---|---|---|
| `audio_engine` | 29 | Remplacer la dépendance `bsp_jc4880` (PCM5102A I2S
MAIN + handles) par `bsp_jc1060p470` (ES8311 via esp_codec_dev, déjà dans le
BSP). Le FLX4 headphone cue (USB isochrone TinyUSB) tourne déjà sur jc1060.
minimp3/dr_flac/WAV portables tels quels. |
| `beat_jump` | 1 | Dépend `library` ✓ (niveau 2). |
| `audio_recorder` | 7 | ⚠️ Dépend du stockage SD — **pas de carte SD dans le
JC1060P470C** (slot absent, 0x107 = bruit attendu). Reporter. |

## Niveau 4 — Deck / contrôle (décision d'architecture à prendre)

| Composant | Sources | Notes |
|---|---|---|
| `deck_core` | 2 | Moteur d'état dual-deck. Dépend control_link/audio_engine/
library. Prêt dès le niveau 3. |
| `control_link` | 5 | UART 0xA5 vers le S3 externe. **Le JC1060P470C n'a pas
de S3 embarqué** — le DDJ est déjà piloté en direct par TinyUSB (usb_tu_app.c).
Deux options : (a) garder le modèle événementiel et faire un pont TinyUSB→
`deck_core_queue_event()` sans UART, (b) migrer control_link tel quel si le S3
externe reste dans le setup. **À décider avant la migration.** |
| `controller_profile_manager` | 1 | Profils S3 (.s3bin) via control_link —
pertinent seulement si (b). |

## Niveau 5 — UI (le gros morceau)

| Composant | Sources | Adaptation requise |
|---|---|---|
| `ui` | 30 | 800×480 → **1024×600** (tous les layouts), DSI + PPA (le BSP
jc1060 fournit le framebuffer), vérifier le placement PPA waveform sur le
nouveau backend, backlight PWM GPIO23 ✓. |

## Niveau 6 — Réseau / OTA / web

| Composant | Sources | Notes |
|---|---|---|
| `p4_ota`, `p4_ota_pull_core` | 4 | OTA signée : portables (common ota_manifest
✓). |
| `p4_ota_pull` | 1 | ⚠️ Dépend `wifi_link` + `wifi_transition_lease` — adapter
le transport à **ETH** (ou abstraire : netif quelconque). |
| `web_server` | 5 | Dépend quasi tout — en dernier. `mdns` + `esp_http_server`
portables ; marchera sur ETH (netif déjà prêt). |
| `wifi_link`, `wifi_transition_lease` | 3 | ⚠️ **Parké** — RPC hang esp_hosted
sous IDF 6.0.2 avec C6 2.12.12 (IDF 6 jamais testé upstream, CI = 5.3–5.5).
Reprendre après : build jetable IDF 5.5.5 (test V9) ou fix upstream. Dossier :
`docs/recherche/esphome-espcontrol-vs-host-idf6-rpc-hang-diff.md`. |

## Ne PAS migrer

- `bsp_jc4880` — spécifique à l'ancienne board (ST7701/GT911/PCM5102A/SDMMC
  GPIO39-44). Le JC1060 a son propre `bsp_jc1060p470`.
- `monitor_pcm_link` — couplé au layout I2S de l'ancienne board (unité 0 vers
  S3). Réévaluer quand l'audio ES8311 du jc1060 sera cadré.

## Ordre recommandé

1. Niveau 1 (fondations) — trivial, aucune surprise.
2. Niveau 2 — après arbitrage USB (MSC vs TinyUSB DDJ).
3. Niveau 3 — audio_engine sur ES8311.
4. Niveau 4 — après la décision control_link.
5. Niveau 5 — UI 1024×600.
6. Niveau 6 — OTA/web sur ETH, puis Wi-Fi quand débloqué.

## Vérification par étape

Chaque niveau : build main-deck-jc1060 + test sur hardware (écran log comme
canal de debug, console TCP:2333 quand l'ETH a une IP) + host tests
(`tests/run_p4_host_tests.ps1` reste le gate des parsers).
Commit+push à chaque jalon (convention repo).
