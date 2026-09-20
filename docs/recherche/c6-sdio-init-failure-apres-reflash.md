# C6 muet sur SDIO après reflash 2.3.0 → 2.12.12 (EspControl) — recherche et plan d'action

**Date :** 2026-09-20 · **Contexte :** Pajoniiir, ESP32-P4 JC4880P443C_I_W (C6 sur SDIO, pins 14-19, reset GPIO54 actif-bas), ESP-Hosted host 2.12.0, IDF 6.0.2.
**Symptôme :** boucle `sdmmc_io_rw_extended ... returned 0x107` (ESP_ERR_TIMEOUT sur CMD52/53) → `H_SDIO_DRV: card init failed even after slave reset` → `ensure_slave_bus_ready failed` → abort hosted « Init event not received within timeout ».

**Constat clé pour le diagnostic :** l'échec se produit au niveau *card init* SDIO, donc AVANT tout échange RPC esp-hosted. Un simple mismatch de version host/slave donnerait plutôt `Received INIT event` + warning de version + échec `esp_wifi_init` (voir issue #215). Ici le C6 ne s'énumère même pas comme carte SDIO : il ne boote pas (app corrompue / mode download / reset bloqué) ou l'hôte le re-reset au mauvais moment (race de boot).

---

## 1. Qu'est-ce qu'EspControl a flashé sur le C6 ? (question 4)

- **EspControl** est un projet de panneau de contrôle no-code basé sur **ESPHome** (jtenniswood/espcontrol, licence PolyForm Noncommercial). Il supporte explicitement la **JC4880P443** (4,3" 480×800 portrait) — c'est bien le même type de carte que celle du projet.
- Le firmware C6 qu'il installe provient du dépôt **esphome/esp-hosted-firmware** : binaires pré-compilés de l'esclave **ESP-Hosted stock + un overlay ESP-NOW** (pont `esp_now_*` sur le canal CustomRpc, injecté pour ESP-Hosted ≥ 2.8.1). Seules les cibles **SDIO** sont compilées (ESP32-C6 SDIO = « Built »).
- **Conclusion : le C6 en 2.12.12 est bien un firmware esp-hosted slave, PAS du EPPP** ni autre chose. 2.12.12 est une version officielle du composant `espressif/esp_hosted` (changelog : « fixed sdio incorrect identification of all-ones bus read », « removed memory leaks caused by mempool »).
- Réserve : l'overlay ESP-NOW est « wire-protocol-coupled » au shim ESPHome côté host. Un host vanilla `esp_hosted 2.12.0` n'utilise pas CustomRpc, donc l'overlay devrait rester inerte — mais le couple officiellement validé par EspControl est « firmware EspControl + C6 assorti », pas « Pajoniiir 2.12.0 + C6 2.12.12 ».
- **EspControl documente officiellement votre symptôme** : page « C6 Wifi Processor Recovery » pour panneaux P4 avec déconnexions répétées, setup Wi-Fi impossible, ou « C6 update timeouts » dans le log USB. La réparation passe par l'USB du P4 (le P4 transfère l'update au C6 en interne via SDIO). ⚠️ Page « If recovery cannot communicate with the C6 » : *un C6 complètement muet (SDIO mort) ne peut PAS être réparé par ce chemin* — il faut alors l'UART de programmation du C6.

Sources : jtenniswood.github.io/espcontrol/getting-started/c6-recovery · /getting-started/install · /getting-started/troubleshooting (« Mismatched or outdated C6 firmware can cause repeated disconnects... ») · github.com/esphome/esp-hosted-firmware (README) · components.espressif.com esp_hosted 2.12.12 changelog.

## 2. Cas connus de C6 silencieux sur SDIO après un reflash (question 1)

| Cas | Symptôme | Cause identifiée | Résolution |
|---|---|---|---|
| **esp-hosted-mcu #127** (M5Stack Tab5, P4+C6 SDIO) | `sdmmc_io_reset: 0x108` / `send_op_cond 0x107` en boucle, `card init failed`, `ensure_slave_bus_ready failed`, abort | Mismatch de versions host/slave + fw slave inadapté | Aligner host et slave (2.6.6 des deux côtés) |
| **esp-hosted-mcu #215** (P4 Function-EV + C6 SDIO) | Transport SDIO OK (« slave: packet, host: packet ») mais `esp_wifi_init` → 258 | Sérialisation RPC de `wifi_init_config_t` incompatible entre 2.12.x (host 2.12.11 vs slave 2.12.3) | Aligner les deux à la même version 2.12.11 → OK |
| **esp-hosted-mcu #143 / #113** (Waveshare 4B, GPIO54 !) | Après slave OTA, `esp_hosted_slave_ota_end` → `ESP_ERR_OTA_VALIDATE_FAILED` (5379) ; **le magic byte 0xE9 du header est corrompu en flash** | QIO forcé dans sdkconfig.defaults slave → bug d'écriture flash (base ESP-IDF) ; corrigé en **2.9.0** (« remove forced QIO ») | Reflash UART direct + erase ; **Espressif : « we do not support rollback »** |
| **esp-hosted-mcu #71** (P4+C6 SDIO) | Juste après un slave OTA : `sdmmc_io_rw_extended 0x107` en boucle côté host, « Unrecoverable host sdio state, reset host mcu » | Le host doit **redémarrer après un slave OTA** (sync issues) ; 2.6.x : « Need to restart host after slave OTA is complete » | Redémarrer/retéléverser le host après OTA du C6 |
| **DFRobot forum (Firebeetle 2 ESP32-P4 / C6-WIFI6)** | `card init failed` dès le boot | C6 **sans firmware esp-hosted** (usine) | Flasher le C6 |
| **Notes locales (skill esp32-p4-bringup)** | `esp_wifi_init` échoue avec boucle 0x107, « Init event not received » | **Race de boot** : l'hôte donne ~1,5 s (15×100 ms de retries) après sa pulsation de reset alors que le C6 a besoin de ~1,7 s pour rebooter | `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY=y` (SDIO uniquement) + power-cycle complet |

Lecture du tableau pour notre cas : un C6 flashé par **transport-OTA** (EspControl met à jour le C6 « en interne » via le P4 = le même mécanisme que #71/#143) qui devient ensuite muet au niveau card-init correspond exactement à (a) l'app C6 corrompue par l'OTA (#143, validation 0xE9), ou (b) un host jamais correctement redémarré/synchronisé après l'OTA (#71), ou (c) le C6 laissé en mode download / en reset par l'outil de flash.

## 3. Le C6 en 2.12.12 attend-il un protocole spécifique ? (question 2)

- **Au niveau transport :** l'INIT event négocie le mode SDIO (packet vs streaming). Le changelog **2.12.0** ajoute « Assert if slave uses SDIO streaming and host as SDIO packet mode ». Les binaires esphome/esp-hosted-firmware sont compilés SDIO ; vérifier que le host P4 est configuré dans le même mode.
- **Au niveau RPC : oui, c'est version-sensible.** #215 prouve une incompatibilité de sérialisation *intra-série* 2.12.x. Entre 2.12.0 et 2.12.12 le RPC a bougé : 2.12.2 (`esp_hosted_get_cp_info`), 2.12.4 (mempool commun, validation des params), 2.12.10 (`esp_wifi_disable_pmf_config`), 2.12.12 (fix « all-ones bus read » SDIO). **Host 2.12.0 + slave 2.12.12 n'est un couple ni testé ni garanti** — la règle officielle (FAQ Espressif / esp-techpedia, warning « Version mismatch: Host [...] > Co-proc [...] ») est d'aligner les versions.
- **Mais** : un mismatch RPC ne peut pas expliquer un échec de *card init* (antérieur à tout RPC). À traiter en second, après avoir rétabli le transport.

## 4. Solutions documentées (question 3)

1. **Recovery EspControl** : page dédiée « Repair C6 and reinstall EspControl » (Chrome/Edge + WebSerial, port USB du P4). Remet le couple EspControl + C6 assorti. ⚠️ Réinstalle aussi le firmware EspControl sur le P4 (à re-flashé ensuite avec Pajoniiir) ; ne fonctionne pas si le C6 est totalement muet sur SDIO.
2. **Reflash direct du C6 (UART)** : procédure officielle « Direct UART Flashing » de l'exemple `host_performs_slave_ota` (esp_hosted 2.12.6+) :
   - construire le slave : `idf.py create-project-from-example "espressif/esp_hosted==2.12.12:slave"` → `idf.py set-target esp32c6` → vérifier `CONFIG_ESP_SDIO_HOST_INTERFACE=y` (défaut sur C6) → `idf.py build` → `network_adapter.bin` ;
   - raccorder UART0 du C6 (TX/RX/EN/IO9-BOOT) à un USB-UART 3,3 V (ou via header PROG_C6) ; mettre le P4 en bootloader pour qu'il ne pilote pas le reset du C6 (`esptool.py --before default_reset --after no_reset run` ou BOOT+RST) ;
   - `esptool.py --chip esp32c6 erase_flash` puis flash complet (bootloader + partition table + `network_adapter.bin`).
3. **OTA par le host** : APIs `esp_hosted_slave_ota_begin/write/end(/activate)` + exemple `host_performs_slave_ota` — uniquement si le transport SDIO est déjà vivant (ce qui n'est pas le cas ici). ⚠️ Nota : la fonction nommée `esp_hosted_flash_with_our_firmware` n'existe pas dans la documentation publique esp-hosted-mcu (recherche sans résultat) ; le mécanisme documenté porte ces noms-là.
4. **Paramètres reset/timeout côté host** : `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY=y` (lâcher le C6 dès le board init et éviter la double pulsation), augmenter le délai post-reset, et surtout **power-cycle complet** (le C6 garde des états à travers les soft-resets).
5. **Downgrade du C6** : pas de rollback supporté côté Espressif (#143) ; un « downgrade » = reflash UART d'un `network_adapter` plus ancien. À éviter sauf besoin de coller un host 2.12.0 exact.

---

## Options d'action classées (la plus probable d'abord)

1. **Power-cycle + race de boot (gratuit, cause documentée)** : débrancher complètement la carte (le C6 garde des états à travers les resets logiciels et peut rester en download mode après un flash WebSerial), rebrancher, et tester avec `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY=y` dans le sdkconfig P4 (l'host ne re-reset plus le C6 et lui laisse le temps de booter avant `esp_wifi_init`).
2. **Diagnostic : le C6 boote-t-il ?** via l'UART du C6 (header) — bannière `fg_mcu_slave: ESP-Hosted-MCU Slave FW version :: 2.12.12` + `SDIO_SLAVE: Using SDIO interface` présentes ⇒ problème host/race/mode ; bannière absente ou boot loop ⇒ C6 corrompu/briké ⇒ passer à l'option 4.
3. **Aligner les versions host ↔ slave** : passer `espressif/esp_hosted` à 2.12.12 dans `firmware/main-deck-p4` (maj `dependencies.lock`, `idf.py reconfigure`, clean `sdkconfig`/`managed_components`), avec `esp_wifi_remote` compatible (≥1.3.1 requis depuis esp_hosted 2.11.0). Cause documentée #215 — mais n'agit pas sur la card-init, donc à faire après rétablissement du transport.
4. **Reflash UART complet du C6 avec erase_flash** + `network_adapter.bin` 2.12.12 construit depuis l'exemple `slave` SDIO (procédure « Direct UART Flashing ») — la voie de recovery si l'app C6 est corrompue (scénario #143) ou en download mode.
5. **EspControl C6 recovery installer** (Chrome/Edge, USB P4, modèle « JC4880P443 ») — voie supportée par l'outil qui a flashé le C6 ; refait l'update C6 en interne. Inconvénients : réinstalle aussi le firmware EspControl sur le P4, et inopérante si le C6 est totalement muet sur SDIO.
6. **Vérifier le mode SDIO packet/streaming host↔slave** (assert introduit en 2.12.0) et, en dernier recours, downgrader le C6 par reflash UART vers 2.12.0 pour coller au host actuel.

## Sources

- https://github.com/espressif/esp-hosted-mcu/issues/127 (Tab5, boucle 0x107/0x108, card init failed)
- https://github.com/espressif/esp-hosted-mcu/issues/215 (incompatibilité RPC intra-2.12.x, résolu en alignant les versions)
- https://github.com/espressif/esp-hosted-mcu/issues/143 et #113 (corruption header 0xE9 lors du slave OTA, QIO forcé, fix 2.9.0, pas de rollback supporté)
- https://github.com/espressif/esp-hosted-mcu/issues/71 (host à redémarrer après slave OTA, sinon boucle 0x107)
- https://github.com/espressif/esp-hosted-mcu/issues/20 (flash initial du slave C6, compat descendante host)
- https://jtenniswood.github.io/espcontrol/getting-started/c6-recovery (recovery C6 officielle EspControl, limites)
- https://jtenniswood.github.io/espcontrol/getting-started/troubleshooting et /install
- https://github.com/esphome/esp-hosted-firmware (binaires C6 = esp-hosted stock + overlay ESP-NOW, SDIO uniquement)
- https://components.espressif.com/components/espressif/esp_hosted/versions/2.12.12/changelog (contenu 2.12.0 → 2.12.12)
- https://components.espressif.com/components/espressif/esp_hosted/versions/2.12.6/examples/host_performs_slave_ota (APIs OTA + « Direct UART Flashing »)
- Forum DFRobot topic 400107 (C6 sans firmware → card init failed)
- Notes internes du projet (skill esp32-p4-bringup) : race de boot 1,5 s vs 1,7 s, `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY=y`, incompatibilité host 2.12 / slave 2.3 (#215-adjacent)
