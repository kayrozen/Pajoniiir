# Analyse — Firmware co-processeur ESP32-C6 « 2.12.12 » (EspControl / ESPHome / ESP-Hosted)

*Recherche documentaire, 2026-09-20. Contexte : hôte ESP32-P4 (ESP-IDF 6.0.2, composant `espressif/esp_hosted` 2.12.0), C6 en SDIO (GPIOs P4-Function-EV-Board : CLK18/CMD19/D0-D3 14-17, Slave_Reset GPIO54). Le C6 a été reflashé de 2.3.0 (usine) vers « 2.12.12 » par l'outil web EspControl ; depuis, l'init transport échoue (`card init failed even after slave reset`, `Init event not received within timeout`, `esp_wifi_init` abort).*

---

## 1. Ce que « 2.12.12 » désigne exactement

**« 2.12.12 » est la version du composant Espressif `espressif/esp_hosted` (ESP-Hosted-MCU)**, pas une version d'ESP-IDF ni d'`esp_wifi_remote` :

- Component Registry : [`espressif/esp_hosted` v2.12.12](https://components.espressif.com/components/espressif/esp_hosted/versions/2.12.12/readme) (uploadée ~1 mois avant le 2026-09-20 ; la dernière stable est 3.0.7, il existe aussi une 2.12.13).
- Changelog officiel 2.12.12 ([source](https://components.espressif.com/components/espressif/esp_hosted/versions/2.12.12/changelog?language=en)) :
  - *« fixed sdio incorrect identification of all-ones bus read »*
  - *« removed memory leaks caused by mempool »*

Le **binaire C6 « 2.12.12 »** flashé par EspControl provient du dépôt [`esphome/esp-hosted-firmware`](https://github.com/esphome/esp-hosted-firmware) (release [`v2.12.12`](https://github.com/esphome/esp-hosted-firmware/releases/tag/v2.12.12), publiée le 2 août 2026, commit `6afc9fa`) :

| Attribut | Valeur |
|---|---|
| Binaire | `network_adapter_esp32c6.bin` ([manifest](https://esphome.github.io/esp-hosted-firmware/manifest/esp32c6.json)) |
| SHA-256 | `bad97ce81e7fcf5f3365898f633b80941ae863db9c754d87a78f89c8f61f2e94` |
| Base | Exemple `slave` de `espressif/esp_hosted==2.12.12` (règle CI : tag `vX.Y.Z` ⇒ composant `X.Y.Z`) |
| ESP-IDF du slave | **v5.5.5** (container CI `espressif/idf:v5.5.5` ; le README cite v5.5.2 pour un build local) — [workflow](https://github.com/esphome/esp-hosted-firmware/blob/main/.github/workflows/build.yml) |
| Overlay | **ESP-NOW-over-CustomRpc** injecté par `apply-overlay.sh` pour esp_hosted ≥ 2.8.1 (canal « peer data transfer ») — [README](https://github.com/esphome/esp-hosted-firmware#esp-now-support) |
| Contenu | Wi-Fi + BT (VHCI), config par défaut de l'exemple `slave` Espressif (GPIO SDIO standard, SDIO streaming) |

Ce n'est donc **ni** une version `esp_wifi_remote` (composant *hôte* uniquement), **ni** la version IDF du slave (v5.5.5). Les versions du dépôt esphome/esp-hosted-firmware sont taguées « avec la version ESP-Hosted utilisée » ([Versioning](https://github.com/esphome/esp-hosted-firmware#versioning)).

### D'où EspControl tire-t-il ce firmware ?

- EspControl est un panneau **basé sur ESPHome** ; sa [privacy policy](https://jtenniswood.github.io/espcontrol/reference/privacy) confirme : *« Some builds also check the public ESPHome hosted firmware manifest for an ESP32-C6 co-processor »* — c'est-à-dire `https://esphome.github.io/esp-hosted-firmware/manifest/esp32c6.json`.
- Le composant ESPHome [`update/esp32_hosted`](https://esphome.io/components/update/esp32_hosted/) détecte la version courante du co-processeur (RPC `GetCoprocessorFwVersion`, étendu en 2.12.2 pour renvoyer aussi nom de puce/chip id) et **sélectionne la version la plus haute ≤ à la version de la lib hôte compilée dans ESPHome**.
- ESPHome pîne actuellement côté hôte ([`components/esp32_hosted/__init__.py`](https://github.com/esphome/esphome/blob/dev/esphome/components/esp32_hosted/__init__.py), branche dev) : `espressif/esp_hosted == 2.12.12`, `espressif/esp_wifi_remote == 1.6.3`, `espressif/wifi_remote_over_eppp == 0.3.3`, `espressif/eppp_link == 1.1.5`. D'où la proposition exacte de « 2.12.12 » par EspControl.

---

## 2. Qu'est-ce que le « 2.3.0 » d'usine, et que change 2.12.x ?

- Le « esp_hosted v2.3.0 (factory) » est un firmware esclave construit depuis le tag 2.3.0 d'[espressif/esp-hosted-mcu](https://github.com/espressif/esp-hosted-mcu) (changelog 2.3.0 : *« Refactored common and port specific code »*). **Ce n'est pas une version du composant ESP-IDF** : `espressif/esp_hosted` 2.3.0 n'existe pas au Component Registry (404), seulement comme tag git / builds d'usine (ex. CrowPanel, cf. [thread HA community](https://community.home-assistant.io/t/esp32-c6-co-processor-firmware-on-the-crowpanel-7-esp32-p4/986735)).
- **Aucun changement cassant du protocole de transport SDIO n'est documenté entre l'ère 2.3.0 et 2.12.12** : le header de trame (`if_type/flags/len/offset/checksum/seq_num`) est stable sur toute la série 2.x ([readme §7](https://components.espressif.com/components/espressif/esp_hosted/versions/2.12.12/readme)). Preuve empirique : votre hôte **2.12.0 fonctionnait avec le slave 2.3.0** avant la mise à jour (Wi-Fi s'associait, reasons 203/205 seulement). La compat croisée hôte/esclave dans 2.x est donc large ; l'échec actuel n'est **pas** un simple mismatch de protocole hôte 2.12.0 ↔ esclave 2.12.12 (même série 2.12.x, le couple le plus proche possible).

### Changements notables 2.3.0 → 2.12.12 (changelog officiel)

| Version | Changement pertinent host/slave |
|---|---|
| 2.4.0 / 2.4.3 | Wi-Fi Enterprise, DPP (RPC supplémentaires) |
| 2.5.2 | Changement du BT controller côté co-processeur (migration guide dédiée) |
| 2.9.x | APIs `esp_hosted_bt_controller_*` (nouveau chemin BT) |
| 2.10.0 | GPIO Expander (contrôle des GPIO du slave via le transport) |
| **2.11.0** | ⚠️ **« must be used with wifi-remote component v1.3.1 or greater »** côté hôte |
| **2.12.0** | ⚠️ **Assert si le slave est en SDIO streaming et l'hôte en SDIO packet mode** ; validation des images OTA du slave ; OTA refusée si mauvais SPI flash mode |
| 2.12.1 | Fix validation d'image OTA sur IDF 6.0+ |
| 2.12.8–2.12.11 | Mempool (option PSRAM), **fix « SDIO deinit tearing down the shared SDMMC host »** |
| **2.12.12** | **Fix « sdio incorrect identification of all-ones bus read »** + fuites mempool — touche précisément le chemin d'init/lecture SDIO |

### Le bug connu de 2.12.12 : issue [#223 « 2.12.12 boot loops (EHM-255) »](https://github.com/espressif/esp-hosted-mcu/issues/223)

Symptômes **quasi identiques aux vôtres** (P4 hôte, GPIO54, 40 MHz) : `card init success` puis `sdmmc_io_rw_extended: sdmmc_send_cmd returned 0xffffffff` (lecture « all-ones »), `H_SDIO_DRV: failed to read data`, reset du slave via GPIO54, `Host is resetting itself, to avoid any sdio race condition`. L'auteur relie explicitement le plantage au changement 2.12.12 (gestion des lectures all-ones) et le fix-ladder cité est le **rollback à 2.12.11** (lib hôte + firmware esclave). Issue ouverte le 2 août 2026, fermée « completed » le 6 août 2026 (contournement rollback, pas de fix dans une note de release dédiée). Une 2.12.13 existe avec de nombreux fixes transport/RPC, et un autre issue ouvert mentionne un RX bloqué sur 2.12.13 (P4 + C6) — la série 2.12.x SDIO sur P4 est encore agitée.

**Côté esclave, le firmware ESPHome diffère aussi du 2.3.0 d'usine** : ESP-IDF 5.5.5 vs ~5.3/5.4 d'époque, overlay ESP-NOW (canal CustomRpc additionnel), config par défaut de l'exemple `slave`. Si votre hôte 2.12.0 est en SDIO *packet mode* (ou un `sdio_rx_mode` différent), le changement de comportement esclave + l'absence du fix all-ones côté hôte 2.12.0 sont les deux candidats les plus plausibles pour expliquer `card init failed`.

---

## 3. Tableau de compat hôte (composant IDF) ↔ firmware esclave

| Firmware esclave C6 | Hôte esp_hosted recommandé | Remarques |
|---|---|---|
| 2.3.0 (usine) | 2.12.0 ✔ (votre config avant) | Protocole SDIO 2.x compatible ; worked before |
| **2.12.11** | 2.11.x–2.12.x | Dernier esclave **avant** le fix all-ones ; état connu bon ; dispo dans le manifest ESPHome (`77d325ac…`) |
| **2.12.12** | 2.12.12+ (le fix all-ones est aussi dans la lib hôte) | Votre combo actuel : hôte 2.12.0 **sans** le fix ↔ esclave 2.12.12 — combo à risque documenté (issue #223) |
| 2.12.13 | 2.12.13 | Nombreux fixes transport/RPC ; un issue RX ouvert P4+C6 |
| 3.0.x (3.0.7 stable) | 3.0.x + `esp_wifi_remote` ≥ 1.3.1 (ESPHome utilise 1.6.3) | Restructuration examples (`wifi_hosted_hci`) ; nécessite de valider IDF 6.0.2 côté hôte |

Règle ESPHome de sélection : firmware esclave ≤ version de la lib hôte compilée. Avec votre hôte **2.12.0**, un EspControl/ESPHome à jour ne proposerait 2.12.12 que si **sa propre** lib hôte est ≥ 2.12.12 (ESPHome dev pîne 2.12.12) — la version affichée par EspControl reflète son build ESPHome, pas le vôtre.

---

## 4. Recommandations pour Pajoniiir

1. **Revenir à un état connu** : reflasher le C6 en **2.12.11** — soit via EspControl (le manifest liste 2.12.11), soit en flashant directement `https://esphome.github.io/esp-hosted-firmware/v2.12.11/network_adapter_esp32c6.bin` (SHA-256 `77d325ac93384e593548f135439f7a5821608f034f8aba8ab097065c4ec9cbac`) par UART/USB du C6. Attendu : retour au comportement « transport OK » de l'ère 2.3.0 avec l'hôte 2.12.0.
2. **Si l'objectif est de rester sur 2.12.x** : aligner les deux côtés — monter l'hôte P4 à `esp_hosted` **2.12.13** (dernier 2.12.x, contient le fix all-ones hérité + fixes SDIO/RX) avec `esp_wifi_remote ≥ 1.3.1` (référence ESPHome : 1.6.3), puis reflasher l'esclave 2.12.13 (pas encore dans le manifest ESPHome ⇒ binaire à construire soi-même depuis `espressif/esp_hosted==2.12.13:slave`, IDF 5.5.x, exemple `slave`).
3. **À éviter** : le combo actuel hôte 2.12.0 (sans fix all-ones) + esclave 2.12.12 ; c'est le seul changement entre « ça marchait » et « ça ne marche plus », et il correspond au bug documenté EHM-255/#223.
4. Ne pas conclure à un problème matériel/SDIO : 20 MHz « pire » et reset GPIO54 corrects ne contredisent pas l'hypothèse logicielle ; l'issue #223 montre les mêmes échecs à 40 MHz sur matériel valide.

## Sources

- https://components.espressif.com/components/espressif/esp_hosted/versions/2.12.12/readme — composant 2.12.12, dépendances (`esp_wifi_remote`, protobuf-c), header transport §7
- https://components.espressif.com/components/espressif/esp_hosted/versions/2.12.12/changelog?language=en — changelog complet 2.1.11→3.x (2.11.0 wifi-remote ≥1.3.1 ; 2.12.0 assert streaming/packet ; 2.12.12 fix all-ones)
- https://components.espressif.com/components/espressif/esp_hosted/versions/2.12.13/changelog?language=en — 2.12.13 (latest 2.12.x)
- https://github.com/esphome/esp-hosted-firmware — binaires pré-construits, overlay ESP-NOW, versioning = version ESP-Hosted
- https://github.com/esphome/esp-hosted-firmware/releases/tag/v2.12.12 — release v2.12.12 (2026-08-02)
- https://esphome.github.io/esp-hosted-firmware/manifest/esp32c6.json — manifest C6 (2.8.0→2.12.12, 3.0.5 ; pas de 2.3.0) + SHA-256
- https://github.com/esphome/esp-hosted-firmware/blob/main/.github/workflows/build.yml — CI : `espressif/esp_hosted==<tag>`, IDF v5.5.5, exemple `slave`, overlay ≥2.8.1
- https://esphome.io/components/update/esp32_hosted/ — mécanisme d'update et règle de sélection de version
- https://esphome.io/components/esp32_hosted/ — composant hôte ESPHome (SDIO 400kHz–50MHz, défaut 40MHz, use_psram)
- https://github.com/esphome/esphome/blob/dev/esphome/components/esp32_hosted/__init__.py — pins hôte ESPHome : esp_hosted 2.12.12, esp_wifi_remote 1.6.3, eppp_link 1.1.5
- https://jtenniswood.github.io/espcontrol/reference/privacy — EspControl consulte le manifest ESPHome C6
- https://github.com/espressif/esp-hosted-mcu/issues/223 — EHM-255, boot loop 2.12.12, symptômes et rollback 2.12.11
- https://community.home-assistant.io/t/esp32-c6-co-processor-firmware-on-the-crowpanel-7-esp32-p4/986735 — « esp_hosted v2.3.0 (factory) »
