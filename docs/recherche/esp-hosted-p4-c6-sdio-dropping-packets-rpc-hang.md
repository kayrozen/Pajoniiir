# ESP-Hosted P4 (host IDF 6.0.2, esp_hosted 2.12.13) + C6 slave SDIO — recensement des cas « Dropping packet(s) from stream » / « Failed to push data to rx queue » / RPC hang / assoc fail 203

**Date de recherche :** 2026-09-20
**Périmètre :** issues/discussions `espressif/esp-hosted-mcu` (+ issues liées `esphome/esphome`, `esp32.com`, Reddit r/esp32, Home Assistant Community, dépôts communautaires). Chaque affirmation est sourcée ; les points non vérifiés sont explicitement marqués.

---

## 1. Signification exacte des symptômes

| Message | Origine | Sens vérifié |
|---|---|---|
| `H_SDIO_DRV: Dropping packet(s) from stream` / `Dropping packet` | host, driver SDIO esp-hosted | L'hôte reçoit des données SDIO mais ne peut pas les pousser dans la file RX (ou les rejette en streaming). Visible aussi côté #199 (EHM-221) dès `esp_hosted_connect_to_slave()`. |
| `H_SDIO_DRV: Failed to push data to rx queue` | host | Conséquence directe du drop : file RX saturée/corrompue. Les deux messages apparaissent toujours ensemble dans les cas recensés. |
| RPC sans réponse (`Timeout waiting for Resp for Req[0x…]`) | host, couche RPC | Le slave ne répond plus aux RPC (scan/connect passent par le RPC). Peut être causé par (a) mismatch de versions host↔slave (warning explicite au handshake), (b) slave wedge SDIO/RX, (c) bug de réassemblage RPC. |
| `Disconnected … reason=203` (`WIFI_REASON_ASSOC_FAIL`) | wifi | Échec d'association non spécifique. Dans le cas esphome#10956, c'est la **conséquence** des drops SDIO pendant l'association (paquets de handshake perdus) → timeout → `Association Leave` → `Restarting adapter`, en boucle. Source : esphome/esphome#10956. |
| `sd_host_wait_for_event returned 0x107` / `sdmmc_send_cmd returned 0xffffffff` | IDF sdmmc | 0x107 = timeout du host SDMMC ; 0xffffffff = lecture « all-ones » du bus (slave ne conduit pas le bus). Lien direct avec EHM-252/#220 et EHM-255/#223. |

**Point important pour ce projet :** EspControl fonctionne avec le même C6 → le handshake de version et le chemin RPC de base fonctionnent. Les symptômes apparaissent sous charge WiFi (scan/association/data) → signature des bugs du chemin RX streaming (#144, #210, #199) et/ou du mismatch fin de version 2.12.12 (slave ESPHome) vs 2.12.13 (host) — pas d'un lien SDIO mort dès le boot (#121/#167).

---

## 2. Recensement des cas connus (P4+C6 et proches)

### 2.1 Cas symptomatiques identiques (drops + assoc 203)

**[A] esphome/esphome#10956** — Waveshare ESP32-P4 Smart 86 Box, P4 host + C6 SDIO (GPIO54 reset active-low, 40 MHz), ESPHome 2025.9.1/2025.9.2, IDF 5.4.2.
- Symptômes exacts : `Dropping packet(s) from stream` + `Failed to push data to rx queue` → timeout → `reason='Association Leave'` → `wifi: Restarting adapter`, en boucle ; parfois assert `netif_add` et Guru Meditation.
- Cause identifiée : **mismatch version slave C6 vs composant esp_hosted épinglé par ESPHome**. ESPHome épingle la version du composant (à l'époque 2.0.11 ; aujourd'hui `esp_hosted==2.12.12` + `esp_wifi_remote==1.6.3` sur `dev`).
- **Fix confirmé par l'auteur du ticket** : flasher le slave C6 avec la version correspondante (`idf.py create-project-from-example "espressif/esp_hosted^<version épinglée>:slave"`, cible esp32c6). Ticket fermé le jour même.
- Note : le log de transport affiche l'avertissement canonique `Version mismatch: Host [x] > Co-proc [y] ==> Upgrade co-proc to avoid RPC timeouts`.
- Source : https://github.com/esphome/esphome/issues/10956

**[B] Home Assistant Community — Waveshare esp32-p4-wifi6-poe-eth** (fév. 2026) : mêmes erreurs (`Failed to push data to rx queue`), renvoyé vers le cas [A]. Le YAML joint utilise la mise à jour C6 via manifeste ESPHome.
- Source : https://community.home-assistant.io/t/waveshare-esp32-p4-wifi6-poe-eth/985217

**[C] Guition JC1060P470 (P4+C6, 7") — « WiFi scans OK, connect always fails — solved »** (juil. 2026) :
- Symptômes : scan OK, MAC C6 lisible, `WiFi.begin()` échoue toujours (statut 6 / WL_DISCONNECTED) — équivalent fonctionnel du blocage scan/connect.
- Cause : firmware C6 usine ancien (build `network_adapter` du 26 août 2025, IDF v5.5).
- **Fix confirmé** : reflash de la seule application C6 (`ota_0` @ 0x10000) avec le `network_adapter_esp32c6.bin` officiel via le **header UART du C6** (TX/RX/GND + GPIO109=BOOT à la masse + EN momentané) → WiFi connecte immédiatement, RSSI -36 dBm. Piège matériel : alimenter la carte via le port USB « HIGH SPEED » (le port UART coupe le header C6).
- Source : https://community.home-assistant.io/t/guition-esp32-p4-with-esp32c6-co-processor-jc1060p470-yellow-wifi-fails-to-connect-solved/1016545

**[D] CrowPanel 7" (Elecrow) / slave usine esp_hosted v2.3.0** — lboshuizen + Reddit 1r5i6ol :
- Le slave usine **v2.3.0** a des bugs de transport SDIO : WiFi tombe après ~4 min sans récupération ; payload OTA corrompu par ce transport (« il faut le fix pour installer le fix »).
- **Fix confirmé** : application ESP-IDF autonome qui met le C6 de 2.3.0 → 2.9.7 par SDIO **sans démarrer WiFi** (1-bit, 10 MHz, chunks 1,5 Ko, ~15 s). Sources : https://github.com/lboshuizen/crowpanel-p4-c6-sdio-ota ; https://community.home-assistant.io/t/esp32-c6-co-processor-firmware-on-the-crowpanel-7-esp32-p4/986735 ; https://www.reddit.com/r/esp32/comments/1r5i6ol/
- Prérequis documenté : la **récupération de transport SDIO** (détection + réinit) n'existe qu'à partir de **2.9.4 sur host ET slave**.

### 2.2 Crashes/blocages du chemin RX streaming (P4+C6)

**[E] #144 (EHM-156)** — assert `sdio_rx_get_buffer sdio_drv.c:830` ou `transport_drv_sta_tx transport_drv.c:267` pendant téléchargements HTTP en boucle ; esp_hosted/slave 2.6.0, IDF 5.5.1 ; reproductible sur PCB custom (LVGL/DSI + MQTT + mDNS), pas sur EV board. **Ouvert.**
- Source : https://github.com/espressif/esp-hosted-mcu/issues/144

**[F] #167 (EHM-188) + #121 (EHM-124)** — `sdmmc_host_wait_for_event returned 0x107` → `sdio_write_task: Failed to send data` → `Unrecoverable host sdio state` ; esp_hosted 2.11.6 (Arduino 3.7.7/IDF 5.5.2), silicon P4 v1.3 ; Waveshare ET 4D Systems, 51k/47k pull-ups, 20 MHz testé ; parfois dès le boot, parfois après des heures. Position Espressif : cause probablement matérielle (signal integrity), non reproduite sur EV board. **Ouvert.**
- Sources : https://github.com/espressif/esp-hosted-mcu/issues/167 ; /issues/121

**[G] #184 (EHM-206)** — TCP entrant se bloque après ~100 Ko (P4+C6, lié à #167) ; un utilisateur contourne en **FreeRTOS mono-cœur** ; Espressif suspecte l'alimentation de la carte et n'a pas reproduit sur Waveshare P4-WIFI6 ni EV board (flux continu 4 Mo OK ~2,5 Mo/s). **Ouvert.**
- Source : https://github.com/espressif/esp-hosted-mcu/issues/184

**[H] #210 (EHM-235)** — double-libération `tlsf_free` dans `sdio_process_rx_task` en **streaming ET packet mode** (2.12.9 des deux côtés, 40/20 MHz, ~2 boots sur 13) ; en packet mode : slave cesse de répondre aux RPC après 30–80 s (`Timeout waiting for Resp for Req[0x101]`, `esp_wifi_set_ps`…). Pistes Espressif : **esp_wifi_remote < 1.3.1 associé à esp_hosted ≥ 2.11.0 double-libère le payload RX** (exigence : wifi-remote ≥ 1.3.1, cf. changelog 2.11.0) + fix `fix(sdio): avoid heap corruption on transport deinit` (commit 98dea22, livré en **2.12.10** « fixed SDIO RX heap corruption on transport deinit and hardened RX buffer allocation »). Fermé par ce commit.
- Source : https://github.com/espressif/esp-hosted-mcu/issues/210

**[I] #199 (EHM-221)** — `esp_hosted_connect_to_slave()` échoue avec **flash encryption activée + `ESP_HOSTED_MEMPOOL_PREFER_SPIRAM`** : `Dropping packet` + `Failed to push data to rx queue` puis corruption mémoire (assert tlsf). esp_hosted 2.12.8, wifi-remote 1.6.0, IDF 5.5.4. **Ouvert.** Pertinent si PSRAM mempool est activé (ESPHome `use_psram`).
- Source : https://github.com/espressif/esp-hosted-mcu/issues/199

### 2.3 Lectures bus all-ones et resets host (famille 2.12.11/2.12.12)

**[J] #220 (EHM-252)** — `sdio_read_regs()` rapporte un **succès** alors que les données reviennent toutes à 1 (0xffffffff en payload) : le lien meurt sans qu'aucune erreur ne soit déclarée, l'hôte ne se réinitialise jamais ; l'auteur a dû écrire son propre watchdog applicatif (liveness + round-trip RPC, 60 s). Se produit sous transfert entrant soutenu (296 Mo–5 Go). Espressif : « recently known issue, we would fix this shortly, in 2.12.12 ». **Fix livré en 2.12.12** (« fixed sdio incorrect identification of all-ones bus read »).
- Sources : https://github.com/espressif/esp-hosted-mcu/issues/220 ; changelog 2.12.12.

**[K] #200 (EHM-222)** — même fonction, chemin inverse : `sdmmc_io_rw_extended: returned 0xffffffff` (échec de **commande**) → `failed to read registers` → « Host is resetting itself » (réinitialisation de l'hôte pour éviter la race SDIO).

### 2.4 Erreurs spécifiques au host P4 sous IDF 6

**[L] #181 (EHM-203)** — `net80211: OS adapter function version error! Version 8 is expected, but it is 0` + échec init Wi-Fi sur host P4 (IDF 6.0.x, esp_hosted 2.12.3). **Cause : `CONFIG_ESP_HOST_WIFI_ENABLED=y` sur le host P4 alors qu'on utilise le Wi-Fi distant** — il ne faut PAS activer le Wi-Fi natif du host. Fermé. ⚠️ Directement pertinent pour un host P4 sous IDF 6.0.2.
- Source : https://github.com/espressif/esp-hosted-mcu/issues/181

### 2.5 OTA slave cassée / versions

**[M] #143 (EHM-155)** — slave **2.7.0** rejette toute OTA (`esp_hosted_slave_ota_end()` → `ESP_ERR_OTA_VALIDATE_FAILED`, réponse RPC 0x1503/5379) vers 2.7.4 comme 2.8.5, alors que 0.0.0→2.7.0 avait marché. Espressif : bug dans `esp_flash` (IDF) sur transition **QIO→QIO** (>2.0.x) ; transition **DIO (≤2.0.x) → QIO (>2.0.x) OK** ; rollback ≤2.0.X impossible depuis QIO. Pas de rollback officiel : « le slave doit accepter toute image, c'est à l'hôte de choisir ». Fermé « Not planned ». Règle pratique reprise ailleurs : **builds C6 en flash mode DIO, éviter QIO** (cf. aussi lboshuizen).
- Source : https://github.com/espressif/esp-hosted-mcu/issues/143

**[N] #229** — OTA par partition du host n'hérite pas du port série ; #77 (EHM-80) : le trigger auto « flash slave au boot en cas de mismatch » était prévu ; #179 (EHM-200) : le **flash UART direct du C6 depuis le P4** est planifié comme fonctionnalité logicielle d'esp-hosted (pas encore livrée à notre connaissance).
- Sources : /issues/229, /issues/77, /issues/179

---

## 3. Le bug #223 (EHM-255) — ce qu'il dit EXACTEMENT

Titre : **« 2.12.12 boot loops (EHM-255) »**, ouvert le 2 août 2026 par savenlid, PCB custom P4 (silicon 3.1) + slave **C5**, host passé de 2.12.11 → **2.12.12**.

Faits exacts issus du ticket :
1. Après l'upgrade host 2.12.11→2.12.12, boot loop : `sd_host_wait_for_event returned 0x107` → `sdio_read_task: Failed to read data - -1 838860 838860` → `Not able to connect with ESP-Hosted slave device` → reset slave via GPIO54 → `sdmmc_io_rw_extended: returned 0x107` → `failed to read registers` → **« Host is resetting itself, to avoid any sdio race condition »** → redémarrage du P4 → boucle. Un reset matériel du P4 a arrêté la boucle.
2. Le commentaire d'analyse (repris du changelog) : 2.12.12 contient « fixed sdio incorrect identification of all-ones bus read » et « removed memory leaks caused by mempool ». Hypothèse : le lien produit marginalement des lectures all-ones ; 2.12.11 les tolérait/mal identifiait (WiFi marchait), **2.12.12 les identifie maintenant comme échec** → chemin fatal de reset du host → boot loop. Le bug ne crée pas la marginalité SDIO, il rend le symptôme fatal. (Cette analyse est celle de l'auteur, assistée par LLM — pas une affirmation Espressif.)
3. Réponse Espressif (mantriyogesh, 3 août 2026) : **non reproduit** (« tried with 100 or 1000 [Hz] both ») ; demande de vérifier GPIOs/reset slave/binaires+sdkconfig. L'auteur précise : intermittent, arrivé une fois juste après une OTA du P4 (image poussée en PSRAM via curl) ; il est repassé host en 2.12.11, puis 30+ OTA sans problème.
4. Workaround de facto confirmé dans le fil : **revenir à 2.12.11 (host et slave)** ; la « échelle de correctifs » proposée (par l'auteur) : rollback 2.12.11 / FREERTOS_HZ 100→1000 + intégrité signal SDIO / désactiver le reset host sur échec. Le ticket est **fermé** (label « Status: Opened », pas de fix référencé).
5. Portée : le ticket concerne un slave **C5** ; le mécanisme (all-ones devenus fatals en 2.12.12) s'applique au transport SDIO en général, donc aussi à C6. Aucun cas confirmé P4+C6 dupliquant ce boot loop n'a été trouvé.

Sources : https://github.com/espressif/esp-hosted-mcu/issues/223 ; changelog 2.12.11/2.12.12 ; https://github.com/espressif/esp-hosted-mcu/issues/200 (comportement « host resets itself »).

---

## 4. Différences de protocole transport : slave « stock 2.3.0 » (usine) vs « 2.12.x »

Le protocole est auto-négocié au INIT (features/capabilities, mode SDIO, flow control), mais plusieurs changements de comportement entre 2.3.0 et 2.12.x sont documentés :

| Sujet | 2.3.0 (usine Guition/Elecrow, 2024) | 2.12.x | Source |
|---|---|---|---|
| **Handshake de version** | Présent dès 2.x (INIT → « Identified slave », version lue par l'hôte) | Idem + warning explicite `Version mismatch: Host > Co-proc ==> Upgrade co-proc to avoid RPC timeouts` ; events INIT/HEARTBEAT/TRANSPORT_FAILURE depuis **2.9.4** | logs esphome#10956, changelog 2.9.4 |
| **Streaming vs packet mode SDIO** | Streaming disponible sur la série 2.x (config Kconfig des deux côtés) | Le **mode doit correspondre** : 2.12.0 « Assert if slave uses SDIO streaming and host as SDIO packet mode ». Défaut 2.12.9 : streaming des deux côtés (`SDIO mode: slave: streaming, host: streaming`) ; packet mode = « Always Rx Max Packet size » (host) + désactiver streaming (slave) | changelog 2.12.0, log #184, #210 |
| **APIs OTA** | Pas d'API `begin/write/end/activate` publiques (ajoutées en **2.6.0**) ; l'ancienne API monolithique `esp_hosted_slave_ota` existait avant | 2.6.0 : 4 APIs ; `esp_slave_ota_activate()` **seulement si slave ≥ 2.6.0** (2.9.2 : activation conditionnelle, sinon redémarrer le host) ; 2.12.0 : validation de l'image entrante (IDF ≥ 6.1) + « Allow slave OTA only if correct SPI Flash Mode » | changelog 2.6.0, 2.9.2, 2.12.0 |
| **Tailles/alignements OTA** | n/a | 2.12.5 : **padding d'alignement 16 octets avant le SHA256** — avant, l'OTA par partition envoyait 15 octets de moins → hash mismatch ; chunks OTA typiques 1400–1500 o | changelog 2.12.5, exemple host_performs_slave_ota |
| **Flash mode** | usine ≤2.0.x = DIO | >2.0.x construit QIO ; QIO→QIO OTA cassée (bug IDF `esp_flash`, #143) ; 2.9.0 supprime la dépendance forcée QIO pour les OTA consécutives ; recommandation pratique : **DIO** | #143, changelog 2.9.0 |
| **Flow control Wi-Fi** | présent (seuils ~60/80 %) | inchangé + retry Tx côté coproc (2.12.13 : défaut 1→5 retries, STA ne retry que sur `ESP_ERR_NO_MEM`) | log #184, changelog 2.12.13 |
| **Récupération transport** | **absente** → chute WiFi ~4 min sans récupération (v2.3.0 usine) | **2.9.4+** : détection + réinit (requis des DEUX côtés) | lboshuizen/crowpanel-p4-c6-sdio-ota |
| **CustomRpc « peer data transfer »** | inexistant | ajouté en **2.8.1** (canal CustomRpc) ; l'overlay ESP-NOW d'ESPHome ne s'applique qu'à **≥ 2.8.1** | esphome/esp-hosted-firmware README |
| **Pairage wifi-remote** | n/a (host sans wifi-remote moderne) | depuis **2.11.0** : wifi-remote **≥ 1.3.1 obligatoire** (sinon double-free du payload RX) | changelog 2.11.0, #210 |
| **Buffers PSRAM** | non | 2.12.8 : `ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` (off par défaut) ; 2.12.13 : PSRAM mempool pour SPI-FD/HD | changelog 2.12.8/2.12.13 |
| **Bugs SDIO corrigés en série 2.12** | présents | 2.12.0 : fuites SDIO deinit ; 2.12.10 : corruption heap RX deinit ; 2.12.12 : all-ones bus read + fuites mempool ; **2.12.13 : « RPC serial reassembly stalling the RX datapath when an RPC handler is slow » + réponses RPC mal parsées rapportées comme succès** | changelog 2.12.x |

**Ce que cela implique pour un host 2.12.13** : un slave 2.3.0 (a) est compatible à l'établissement du lien (même famille de protocole), (b) mais sans la récupération de transport, sans activate OTA, avec le chemin OTA/flash-mode fragile — les reproches documentés (« WiFi tombe à ~4 min », « payload OTA corrompu ») viennent de là, pas d'un changement de framing incompatible. Un slave **2.12.12** (ESPHome) a exactement le même protocole de transport que 2.12.13 ; l'écart d'un patch ne déclenche pas d'incompatibilité connue, mais l'avertissement de handshake recommande d'aligner.

---

## 5. Overlay CustomRpc ESP-NOW d'ESPHome (esphome/esp-hosted-firmware) vs host IDF standard

Vérifié (README esphome/esp-hosted-firmware + composant esphome `esp32_hosted/__init__.py`) :
- Les binaires ESPHome = **stock ESP-Hosted slave + petit overlay** qui bridge l'API `esp_now_*` sur le canal **CustomRpc « peer data transfer »** (existe depuis esp_hosted **2.8.1**). L'overlay est injecté par `apply-overlay.sh` à la build ; versions < 2.8.1 = slave stock non modifié.
- Le contrat wire (`slave-overlay/esp_now_hosted_rpc.h`) doit rester **octet-pour-octet identique** entre le shim hôte ESPHome (`esp_now_hosted.cpp`, uniquement compilé sur host P4) et le firmware overlay du coprocesseur.
- Le host ESPHome force `CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y` + 8 handlers CustomRpc ; le coprocesur enregistre 1 handler (REQ), l'hôte 3 (RESP/RECV/SEND).
- **Avec un host IDF standard** : l'overlay n'ajoute des messages que sur le canal CustomRpc ; un host IDF qui n'active pas `ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER` ignore simplement ces messages. Aucun cas documenté d'incompatibilité overlay↔host IDF n'a été trouvé ; en revanche, l'ESP-NOW via hosted ne fonctionne PAS sans cet overlay des deux côtés (upstream : esp-hosted-mcu#19 « Support for ESP Now (EHM-19) », toujours non supporté nativement par Espressif).

---

## 6. Procédures confirmées pour reflasher le slave C6 depuis un host P4

### 6.1 host_performs_slave_ota (OTA par le transport SDIO/SPI/UART) — méthode officielle
Source : exemple `host_performs_slave_ota` (composant espressif/esp_hosted, doc 2.12.6) + guide chvvkumar.
- 4 APIs : `esp_hosted_slave_ota_begin/write/end/activate` + `esp_hosted_get_coprocessor_fwversion()`. Chunks typiques **1400–1500 o**.
- 3 sources de firmware : LittleFS, **partition dédiée du host** (`slave_fw`), HTTPS. Facilité : LittleFS > HTTPS > Partition.
- Vérification de version optionnelle avant OTA ; `activate()` seulement si slave ≥ 2.6.0, sinon **redémarrer le host** pour éviter les désynchro.
- ⚠️ Pièges documentés : image tronquée de 15 o avant 2.12.5 (alignement 16 o/SHA256) ; flash mode QIO→QIO rejeté (#143) → vérifier `Flash mode: DIO` via `esptool.py image_info` ; #229 (port série non hérité).
- Cas réel confirmé : 0.0.6 → 2.7.2 sur Waveshare P4 Touch LCD (guide chvvkumar, déc. 2025).

### 6.2 OTA « sans WiFi » pour slave usine 2.3.0 — méthode lboshuizen (CrowPanel/Guition-like)
- Application ESP-IDF autonome flashée **à la place** du firmware P4 : init SDIO (1-bit, **10 MHz**, ou 5 MHz en secours), firmware C6 lu depuis une partition LittleFS, transfert par chunks **1,5 Ko**, activate, ~15 s. WiFi jamais démarré (le transport 2.3.0 meurt sous WiFi avant la fin de l'OTA).
- Binaire recommandé : `https://esphome.github.io/esp-hosted-firmware/v2.9.7/network_adapter_esp32c6.bin` (SHA256 `c9286c98…f6c8bd11`).
- Source : https://github.com/lboshuizen/crowpanel-p4-c6-sdio-ota

### 6.3 UART direct (récupération, fonctionne même si SDIO est mort)
- Pins nécessaires (confirmé par Espressif dans #143) : **C6_U0TXD, C6_U0RXD, C6_GPIO9 (BOOT), EN** — 4 fils vers le P4 ou un adaptateur USB-UART 3,3 V.
- Procédure confirmée sur **Guition JC1060P470** : header TX/RX/GND du C6 ; **GPIO109 à la masse** (peut rester), **EN à la masse un instant** pour entrer en download ; alimentation via le port USB « HIGH SPEED » (le port UART coupe le header C6) ; ensuite `esptool` en écriture comme en backup. Source : fil HA Guition (§2.1-C).
- Procédure CrowPanel 7" (test pads sous l'écran, V1.0) : P25=GPIO16 TX, P21=GPIO17 RX, P36=GPIO9 BOOT (à GND pendant reset) ; flash `esptool --chip esp32c6 write_flash --flash_mode dio 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 network_adapter.bin` — **base bootloader à 0x0 (RISC-V)**, pas 0x100. Sources : lboshuizen README/HARDWARE.md + HA thread 986735.
- Le flash UART direct P4→C6 piloté par le P4 est **planifié** côté Espressif (#179) mais pas livré.

### 6.4 Par la pile ESPHome (si l'hôte tourne sous ESPHome)
- `update: platform: esp32_hosted` en mode **embedded** (binaire + SHA256) ou **http** (manifeste `https://esphome.github.io/esp-hosted-firmware/manifest/esp32c6.json`) ; le composant choisit « la plus haute version ≤ version de la lib hôte ». Source : https://esphome.io/components/update/esp32_hosted/

### 6.5 Quel firmware choisir pour être compatible host esp_hosted 2.12.13
- Règle du handshake : si Host > Co-proc, l'hôte logue « Upgrade co-proc to avoid RPC timeouts » → viser **slave ≥ version hôte ou égal**.
- ESPHome (branche `dev`) épingle : `espressif/esp_hosted==2.12.12` + `esp_wifi_remote==1.6.3` (host). Les binaires officiels correspondants sont sur `esphome.github.io/esp-hosted-firmware` (tag v2.12.x). ESPHome exige IDF ≥ 5.3 et (depuis 2.11.0) wifi-remote ≥ 1.3.1.
- Pour un **host IDF 6.0.2 à 2.12.13** : le plus sûr est de **flasher un slave 2.12.13** (`idf.py create-project-from-example "espressif/esp_hosted==2.12.13:slave"`, cible esp32c6, flash mode DIO) ou, si le C6 doit garder l'overlay ESP-NOW, d'utiliser le binaire ESPHome **v2.12.12/2.12.13** (overlay additif, compatible hôte IDF standard). Un slave 2.12.12 face à un host 2.12.13 est un écart d'un patch, toléré par le protocole mais non recommandé.
- Options de build slave à connaître : streaming/packet mode (même valeur que l'hôte), `ESP_HOSTED_COPROCESSOR_APP_MAIN`, flash mode DIO.

---

## 7. Options classées pour le cas présent (P4 IDF 6.0.2 / host 2.12.13, C6 slave ESPHome 2.12.12, SDIO GPIO54 active-low 40 MHz)

1. **[Probabilité la plus forte] Aligner finement les versions.** Passer le slave C6 sur 2.12.13 (OTA via `host_performs_slave_ota`, §6.1, ou binaire ESPHome/manifeste) ou repasser le host en 2.12.12. Justification : cas [A] identique (drops + assoc 203 + RPC sans réponse) résolu uniquement par l'alignement ; 2.12.13 corrige en plus un **stall du chemin RX par réassemblage RPC** et des réponses RPC mal parsées (changelog), qui expliquent exactement « RPC scan/connect sans réponse ».
2. **[Obligatoire sous IDF 6] Vérifier la config du host P4** : `CONFIG_ESP_HOST_WIFI_ENABLED` **désactivé** (cas [L], EHM-203) ; wifi-remote **≥ 1.3.1** apparié à esp_hosted ≥ 2.11.0 (cas [H]).
3. **[Si drops sous charge / OTA] Écarter les pièges RX connus** : ne pas activer `ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` + flash encryption ensemble (cas [I]) ; si mempool PSRAM requis (assert `sdio_mempool_create`), l'activer sans encryption flash et tester.
4. **[Réduction de marginalité SDIO]** baisser `CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ` (40 → 20 puis 10 MHz), pull-ups 47–51 kΩ, traces courtes ; envisager packet mode **des deux côtés** si streaming échoue (le mismatch streaming/packet assert depuis 2.12.0). Les cas [F]/[G] montrent toutefois que le drop de fréquence ne suffit pas toujours.
5. **[Si le slave était encore en 2.3.0 usine]** : reflash obligatoire (§6.2) — mais ce n'est pas le cas ici (2.12.12 déjà en place).
6. **[Filet de sécurité]** implémenter la surveillance documentée : events `INIT`/`HEARTBEAT`/`TRANSPORT_FAILURE` (exemple `host_hosted_events`) plutôt qu'un watchdog artisanal ; noter que le « reset host sur échec SDIO » introduit par la détection stricte 2.12.12 (all-ones) peut transformer une erreur passagère en reboot (cas [K]/#223) — un rollback host en 2.12.11 est le workaround confirmé si ce comportement apparaît.
7. **[Dernier recours]** UART direct sur le header C6 (§6.3) — confirmé fonctionnel sur Guition JC1060P470 et CrowPanel.

**Non vérifié / à confirmer localement :** existence d'un cas P4+C6 reproduisant exactement le boot loop EHM-255 (le ticket original est P4+C5) ; comportement précis du handshake 2.12.12↔2.12.13 (aucun ticket trouvé, écart d'un patch) ; date exacte de l'introduction du mode streaming SDIO (antérieure à 2.6, non retrouvée dans les changelogs).

---

## 8. Sources

- https://github.com/esphome/esphome/issues/10956 (cas principal, fix = alignement versions)
- https://github.com/espressif/esp-hosted-mcu/issues/223 (EHM-255), /issues/220 (EHM-252), /issues/200, /issues/121, /issues/167 (EHM-188), /issues/144 (EHM-156), /issues/184 (EHM-206), /issues/210 (EHM-235), /issues/199 (EHM-221), /issues/181 (EHM-203), /issues/143 (EHM-155), /issues/179, /issues/77, /issues/229
- Changelog complet esp-hosted-mcu : https://raw.githubusercontent.com/espressif/esp-hosted-mcu/main/docs/changelog.md (2.6.0, 2.8.1, 2.9.0–2.9.4, 2.11.0, 2.12.0, 2.12.5, 2.12.8, 2.12.10–2.12.13) et pages registry components.espressif.com
- https://github.com/esphome/esp-hosted-firmware (overlay ESP-NOW, CustomRpc, ≥2.8.1) ; https://raw.githubusercontent.com/esphome/esphome/dev/esphome/components/esp32_hosted/__init__.py (pin 2.12.12 / wifi-remote 1.6.3) ; https://esphome.io/components/update/esp32_hosted/
- https://github.com/lboshuizen/crowpanel-p4-c6-sdio-ota (2.3.0→2.9.7 sans WiFi, UART test pads) ; https://github.com/chvvkumar/esp32p4-c6-hosted-firmware-generator (guide OTA 0.0.6→2.7.2) ; https://github.com/tymorton/esp32-p4-c6-espnow-enabler
- https://community.home-assistant.io/t/guition-esp32-p4-with-esp32c6-co-processor-jc1060p470-yellow-wifi-fails-to-connect-solved/1016545 (Guition, UART header, fix confirmé) ; /t/esp32-c6-co-processor-firmware-on-the-crowpanel-7-esp32-p4/986735 ; /t/waveshare-esp32-p4-wifi6-poe-eth/985217
- Reddit r/esp32 : threads 1r5i6ol (CrowPanel 2.3.0) et 1pk1dtm (notes OTA P4→C6) — contenus consultés via extraits de recherche et dépôts liés (accès direct Reddit bloqué aux outils) ; esp32.com t=47208/t=43750/t=46759 — référencés, contenu derrière bot-check.
