# Init Wi-Fi ESPHome sur host esp_hosted (P4+C6) & APIs esp_wifi_remote sur IDF 6

*Recherche web + lecture des sources, 2026-09-20. Contexte : main-deck-jc1060 (ESP32-P4, IDF 6.0.2),
co-processeur C6 sous esp_hosted slave 2.12.12, EspControl (build ESPHome) fonctionne, notre
`wifi_console_start()` (v83) échoue avec reason 203.*

Sources primaires (toutes vérifiées dans `docs/recherche/_fetch/`) :

- ESPHome `dev` : `esphome/components/wifi/wifi_component_esp_idf.cpp`, `wifi_component.cpp`,
  `wifi_component.h`, `esphome/components/esp32_hosted/__init__.py`
- esp-wifi-remote `main` (v1.6.4, fichiers générés `idf_v6.0` — identiques à `idf_v6.1`) :
  https://github.com/espressif/esp-wifi-remote
- esp-hosted-mcu tag **v2.12.12** : `host/api/src/esp_hosted_api.c`, `host/api/src/esp_wifi_weak.c`,
  `slave/main/slave_wifi_std.c`, `host/port/.../port_esp_hosted_host_wifi_config.h`
- Datasheet ESP32-C6 v1.5 (Espressif)

---

## 1) Séquence d'init Wi-Fi complète d'ESPHome (host esp_hosted, STA)

Ordre exact des appels `esp_wifi_*` / `esp_netif_*`, reconstitué depuis
`wifi_component_esp_idf.cpp` + `wifi_component.cpp` (ESPHome dev) :

### Phase A — pré-setup (avant tout le reste)

1. `esp_netif_init()` + `esp_event_loop_create_default()` (composant network)
2. `wifi_pre_setup_()` → `wifi_lazy_init_()` (si `enable_on_boot`) :
   - `esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, ...)`
   - `esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, ...)`
   - `esp_netif_create_default_wifi_sta()` (netif STA créé **avant** `esp_wifi_init`)
   - `wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();` avec `cfg.nvs_enable = 0` si pas de NVS prefs
   - **`esp_wifi_init(&cfg)`**
   - **`esp_wifi_set_storage(WIFI_STORAGE_RAM)`** ← ESPHome utilise **RAM**, jamais FLASH
     (les credentials ne sont jamais écrits en NVS du co-proc ; ESPHome garde les siens dans sa propre
     préférence NVS hôte via `SavedWifiSettings`)

### Phase B — start (WiFiComponent::start())

3. `wifi_sta_pre_setup_()` → `wifi_mode_(true, -)` :
   - **`esp_wifi_set_mode(WIFI_MODE_STA)`**
   - **`esp_wifi_start()`** (seulement si pas déjà démarré)
4. `wifi_apply_output_power_(...)` → `esp_wifi_set_max_tx_power(val)` (seulement si configuré)
5. `wifi_apply_power_save_()` → **`esp_wifi_set_ps(...)`** :
   - `light` (défaut ESPHome) → `WIFI_PS_MIN_MODEM`
   - `high` → `WIFI_PS_MAX_MODEM` ; `none` → `WIFI_PS_NONE`
6. `transition_to_phase_(INITIAL_CONNECT)` puis `start_connecting(params)` → `wifi_sta_connect_(ap)`

### Phase C — connexion (wifi_sta_connect_)

7. `wifi_mode_(true, {})` (idempotent)
8. Construction de `wifi_config_t conf` (zéro-initialized puis champs explicites) :
   - `conf.sta.ssid` / `conf.sta.password` (memcpy, pas de strlcpy)
   - `conf.sta.threshold.authmode` : **`WIFI_AUTH_OPEN` si password vide** ; sinon selon
     `min_auth_mode_` (défaut `WIFI_MIN_AUTH_MODE_WPA2` → `WIFI_AUTH_WPA2_PSK`) — c'est la valeur v83
     que nous avons déjà répliquée
   - `conf.sta.threshold.rssi = -127` (filtre désactivé, « we do our own filtering »)
   - `conf.sta.bssid_set = false` (sauf fast_connect sauvegardé)
   - **`conf.sta.channel`** : si un canal est fixé → `conf.sta.channel = X` ET
     `conf.sta.scan_method = WIFI_FAST_SCAN` ; **sinon (cas général, notre cas) →
     `scan_method = WIFI_ALL_CHANNEL_SCAN`** avec `channel = 0`. ESPHome ne fait de FAST_SCAN que
     quand l'utilisateur épingle un canal ; sans canal il utilise ALL_CHANNEL_SCAN (choix du meilleur
     BSSID après scan complet), pas le « premier AP qui matche ».
   - `conf.sta.listen_interval = 0` (défaut driver = 3 si 0)
   - **`conf.sta.pmf_cfg.capable = true ; .required = false`** (identique à notre v83)
   - pas de `bsi`, pas de `sae_pwe_h2e`, pas de `failure_retry_cnt` touchés
9. `esp_wifi_get_config(WIFI_IF_STA, &current_conf)` — comparaison binaire ; si différent :
   **`esp_wifi_disconnect()`** d'abord
10. **`esp_wifi_set_config(WIFI_IF_STA, &conf)`**
11. `wifi_sta_ip_config_()` → DHCP client (`esp_netif_dhcpc_start/stop` sur le netif hôte)
12. **`esp_wifi_connect()`**

### Phase D — au event `WIFI_EVENT_STA_START` (traité dans `wifi_process_event_`)

13. `esp_netif_set_hostname(s_sta_netif, App.get_name())`
14. **re-apply `wifi_apply_power_save_()`** → `esp_wifi_set_ps()` appelé une 2ᵉ fois après STA_START
15. `wifi_apply_band_mode_()` → `esp_wifi_set_band_mode(band_mode_)` — **uniquement sous
    `#ifdef SOC_WIFI_SUPPORT_5G`**. Sur un host **P4**, `SOC_WIFI_SUPPORT_5G` n'est pas défini
    (le P4 n'a pas de Wi-Fi), donc **ESPHome n'appelle JAMAIS `esp_wifi_set_band_mode`** dans notre
    configuration. `band_mode_` par défaut = `WIFI_BAND_MODE_AUTO`.

### Ce qu'ESPHome n'appelle JAMAIS (avant ou après connect)

- **`esp_wifi_set_country()` / `esp_wifi_set_country_code()`** — aucun appel dans tout le
  composant wifi. Le pays est donc celui du **slave** par défaut IDF : policy `AUTO`
  (`WIFI_COUNTRY_POLICY_AUTO`), CC `"CN"`, canaux 1–13 ; en policy AUTO la config pays de l'AP
  associé est adoptée après connexion. Les canaux 12/13 sont quand même scannés.
- **`esp_wifi_set_protocol()`** — aucun appel (bitmap par défaut du driver slave).
- **`esp_wifi_set_bandwidth()`** — aucun appel.
- **`esp_wifi_set_scan_parameters()`** — aucun appel (timings de scan = défauts IDF du slave :
  scan actif, 0–120 ms/canal).
- **`esp_wifi_set_event_mask()`** — aucun appel.
- `esp_wifi_scan_start()` n'est appelé que pour le roaming (11k/roam check), le captive portal et
  le scanner API — **jamais** dans le chemin de connexion normal (le scan interne du driver fait
  le travail lors de `esp_wifi_connect`).

### Config esp32_hosted (composant ESPHome)

`esphome/components/esp32_hosted/__init__.py` ne touche qu'au transport et aux Kconfig
`CONFIG_ESP_HOSTED_*` (pins SDIO/SPI, bus width, `sdio_frequency` 40 MHz par défaut, reset pin,
`CONFIG_ESP_HOSTED_SDIO_SLOT_n`...). Exige IDF ≥ 5.3 (sinon crash heap double-free, fixé 2.11.0).
Il n'appelle aucun `esp_wifi_*` lui-même ; tout le cheminement Wi-Fi passe par le composant `wifi`.

---

## 2) Le C6 est-il dual-band ? Quelle bande scanne-t-on ?

**Non.** L'ESP32-C6 est **2,4 GHz uniquement** (Wi-Fi 6 / 802.11ax en 2,4 GHz, 1T1R ;
datasheet ESP32-C6 v1.5). Le dual-band Espressif, c'est l'ESP32-C5.

Conséquences :

- Si la box « kayrozen » ne diffuse le SSID que sur 5 GHz, le C6 ne peut physiquement pas la voir
  → raison 201 (`NO_AP_FOUND`), pas 203. Si l'AP 2,4 GHz existe, il est visible.
- ESPHome hôte (P4) n'appelle pas `esp_wifi_set_band_mode` (cf. §1, `SOC_WIFI_SUPPORT_5G` non
  défini sur P4), donc aucune sélection de bande n'est faite côté host — tout va sur la radio 2,4 GHz
  du C6, seul choix possible.
- Même côté API, `esp_wifi_set_band_mode` sur un slave C6 renverrait `ESP_ERR_NOT_SUPPORTED`
  (commentaire esp_hosted : « always available at high level, but returns ESP_ERR_NOT_SUPPORTED when
  co-processor does not support »), même si le RPC `WifiSetBandMode` existe dans esp_hosted 2.12.12
  (`slave/main/slave_wifi_std.c:1858`) et que `H_WIFI_DUALBAND_SUPPORT` est actif sur host IDF ≥ 5.4.
- La raison 203 (`ASSOC_FAIL`) signifie que le AP a été **vu** mais l'association a été rejetée :
  problème auth/PMF/charge, pas bande.

---

## 3) esp_wifi_remote sur IDF 6 : APIs proxifiées vs retours locaux silencieux

Chaîne d'appels réelle sur IDF 6.0.2 + esp_hosted 2.12.x :
`esp_wifi_xxx()` (app de l'app) → wrapper généré `esp_wifi_with_remote.c` (esp-wifi-remote 1.6.x)
→ `esp_wifi_remote_xxx()` → **implémentation forte esp_hosted** (`esp_hosted_api.c` → RPC protobuf)
; si esp_hosted n'implémente pas la fonction, le **weak** `esp_wifi_remote_weak.c` log
`W esp_wifi_remote_weak: <fonction> unsupported` et renvoie **`ESP_ERR_NOT_SUPPORTED`**
(retour local silencieux si les logs de ce tag ne sont pas visibles).

### APIs proxifiées vers le co-proc (implémentées dans esp_hosted 2.12.12, host IDF 6)

init, deinit, set_mode, get_mode, start, stop, restore, connect, disconnect, clear_fast_connect,
deauth_sta, scan_start, set_scan_parameters, get_scan_parameters, scan_stop, scan_get_ap_num,
scan_get_ap_records, scan_get_ap_record, clear_ap_list, sta_get_ap_info, set_ps, get_ps,
set_protocol, get_protocol, set_bandwidth, get_bandwidth, set_channel, get_channel, set_country,
get_country, set_mac, get_mac, set_config, get_config, ap_get_sta_list, ap_get_sta_aid,
set_storage, set_max_tx_power, get_max_tx_power, set_country_code, get_country_code,
sta_get_negotiated_phymode, sta_get_aid, sta_get_rssi, set_inactive_time, get_inactive_time,
set_band, get_band, set_band_mode, get_band_mode, set_protocols, get_protocols, set_bandwidths,
get_bandwidths, set_okc_support, sta_enterprise_enable/disable (+ EAP), itwt/twt (série sta_itwt_*),
set_ant/get_ant/ant_gpio.

### APIs NON proxifiées par esp_hosted 2.12.12 → weak `ESP_ERR_NOT_SUPPORTED`

Vérifié par diff automatique entre les 89 fonctions wrappées par esp-wifi-remote (`idf_v6.0`) et
les implémentations fortes de esp-hosted v2.12.12 (le bloc est **commenté** dans
`esp_hosted_api.c`, lignes 946–984) :

`esp_wifi_80211_tx`, `esp_wifi_register_80211_tx_cb`, `esp_wifi_action_tx_req`,
`esp_wifi_remain_on_channel`, `esp_wifi_config_80211_tx`, `esp_wifi_config_80211_tx_rate`,
`esp_wifi_config_11b_rate`, `esp_wifi_connectionless_module_set_wake_interval`,
`esp_wifi_force_wakeup_acquire`, `esp_wifi_force_wakeup_release`, toute la série FTM
(`ftm_initiate_session`, `ftm_end_session`, `ftm_resp_set_offset`, `ftm_get_report`),
toute la série CSI (`set_csi`, `set_csi_config`, `get_csi_config`), promiscuous
(`set_promiscuous`, `get_promiscuous`, `set_promiscuous_rx_cb`, filters/ctrl_filter),
`set_vendor_ie`, `set_vendor_ie_cb`, `set_event_mask`, `get_event_mask`, `get_tsf_time`,
`set_rssi_threshold`, `statis_dump`, `set_dynamic_cs`, `get_home_channel`.

À noter :
- Toutes ces fonctions non supportées sont **hors du chemin STA connect** — ni ESPHome ni notre
  `wifi_console_start` n'en utilisent. Aucune ne peut expliquer le 203 à elle seule.
- `esp_wifi_get_channel` (que nous utilisons) **est** proxifié ; `esp_wifi_get_home_channel` ne
  l'est pas (piège si on change de call un jour).
- `esp_wifi_scan_get_ap_records` / `get_ap_num` / `sta_get_ap_info` sont proxifiés → notre scan
  diagnostic v77 fonctionne pour de vrai sur les données du C6.
- `esp_wifi_set_event_mask` non supporté : silencieux si quelqu'un l'ajoute un jour.

### Support côté slave 2.12.12

Le slave (`slave/main/slave_wifi_std.c`) traite bien `WifiSetPs`, `WifiConnect`, `WifiSetConfig`,
`WifiScanStart`, `WifiSetProtocol`, `WifiSetStorage`, `WifiSetCountryCode`, `WifiSetCountry`,
`WifiSetProtocols`, `WifiSetBandMode`... Le slave ESPHome/esp_hosted 2.12.12 est donc fonctionnellement
équipé pour tout ce qu'ESPHome envoie.

---

## 4) Reason code 203 : traitement ESPHome vs nous

`203 = WIFI_REASON_ASSOC_FAIL` (« association failed », l'AP a refusé/le handshake 802.11 assoc a
échoué). ESP-IDF également : 200 beacon timeout, 201 no AP found, 202 auth fail, **203 assoc fail**,
204 handshake timeout.

Chez ESPHome (état `dev`, machine à états de retry) :

- `WIFI_EVENT_STA_DISCONNECTED` → log du reason (`get_disconnect_reason_str`), `s_sta_connect_error=true`.
  **Seuls 201 (`NO_AP_FOUND`) et 207 (`ROAMING`) sont traités spécialement** ; 203 suit le chemin
  générique `error_from_callback_` → `retry_connect()`.
- `retry_connect()` / `determine_next_phase_()` : phases `INITIAL_CONNECT` /
  `FAST_CONNECT_CYCLING_APS` (1 tentative/AP) → `SCAN_CONNECTING` (**2 tentatives par BSSID**,
  `WIFI_RETRY_COUNT_PER_BSSID = 2`, avec pénalité de priorité du BSSID à chaque échec) →
  `RETRY_HIDDEN` (1 tentative/SSID) → **`RESTARTING_ADAPTER`** (`esp_wifi_stop` → `esp_wifi_start`,
  saut de phase si captive portal/improv actif) → re-scan complet. Boucle infinie, pas de backoff
  exponentiel, pas de reboot sur échec (sauf `reboot_timeout` global, 15 min par défaut).
- Filet de sécurité : `WIFI_CONNECT_TIMEOUT_MS = 46000` — une tentative bloquée plus de 46 s est
  avortée par `wifi_disconnect_()` + `retry_connect()` (commentaire : ne pas baisser, l'échec réel
  arrive par callback, pas par timeout).
- Lecture clé pour nous : **ESPHome ne reset jamais la config et ne change jamais authmode/PMF entre
  deux essais** ; il répète exactement la même `wifi_config_t`. Si ESPHome connecte avec threshold
  OPEN et nous non, c'est la config, pas la stratégie de retry.

Chez nous (`wifi_console.c`) : event handler sans retry + watchdog 10 s qui rappelle
`esp_wifi_connect()` (v78, pour éviter le hang RPC) + scan diagnostic v77 dans une task jetable.
Différences structurelles vs ESPHome : pas de re-scan cyclique pilotant le choix du BSSID, pas de
restart adaptateur, pas de timeout d'attempt (si le RPC hang, le watchdog rappelle simplement
`connect`), `WIFI_FAST_SCAN` (premier match) au lieu de `ALL_CHANNEL_SCAN`.

---

## 5) Liste exacte des appels à répliquer dans `wifi_console_start`

Séquence minimale fidèle à ESPHome (ce qui manque à notre v83 est marqué ➕) :

```c
// Phase A
esp_netif_init();
esp_event_loop_create_default();
esp_netif_create_default_wifi_sta();
wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
esp_wifi_init(&cfg);
esp_wifi_set_storage(WIFI_STORAGE_RAM);          // ➕ ESPHome = RAM, nous = défaut FLASH
esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, ...);
esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ...);

// Phase B
esp_wifi_set_mode(WIFI_MODE_STA);
esp_wifi_start();                                 // ESPHome: start AVANT set_config
// dans le handler STA_START :
//   esp_netif_set_hostname(netif, "main-deck"); ➕
//   esp_wifi_set_ps(WIFI_PS_MIN_MODEM);        // ➕ re-apply après STA_START

// Phase C (après STA_START)
wifi_config_t conf = { 0 };
// ssid/password, threshold.authmode = WIFI_AUTH_OPEN (password vide -> OPEN),
// threshold.rssi = -127, bssid_set = false, channel = 0,
// scan_method = WIFI_ALL_CHANNEL_SCAN,     // ➕ ESPHome ne met FAST_SCAN qu'avec canal fixé
// listen_interval = 0,
// pmf_cfg = { .capable = true, .required = false }
esp_wifi_get_config(WIFI_IF_STA, &cur);          // (optionnel, comparaison)
esp_wifi_set_config(WIFI_IF_STA, &conf);
esp_wifi_connect();                               // v80 : chez nous via watchdog, OK
```

Différences restantes v83 → ESPHome, par impact décroissant :

1. `scan_method` : nous `WIFI_FAST_SCAN` (channel=0 → premier AP qui matche), ESPHome
   `WIFI_ALL_CHANNEL_SCAN`. En environnement à plusieurs BSSID/maillés, le fast scan peut choisir
   un BSSID qui refuse l'association alors qu'un autre l'accepterait.
2. `esp_wifi_set_storage(WIFI_STORAGE_RAM)` absent chez nous (défaut FLASH → écriture des creds
   en NVS du co-proc à chaque set_config).
3. Ordre `set_mode/set_config/start` : nous faisons `set_mode → set_config → start` ; ESPHome fait
   `set_mode → start → (STA_START) → set_config → connect`. Les deux sont valides en IDF natif,
   mais la séquence ESPHome est la seule validée de bout en bout sur le chemin RPC esp_hosted
   (set_config part vers un slave déjà démarré).
4. `esp_wifi_set_ps` : chez nous jamais appelé explicitement (défaut driver = MIN_MODEM, donc
   équivalent), ESPHome l'applique au boot ET après chaque STA_START.
5. Aucun de nous deux ne touche country/protocol/band_mode/bandwidth : rien à ajouter.

---

## 6) Réponses courtes aux 4 questions

1. **Config avant connect** : set_mode(STA) → start → [STA_START: hostname, set_ps(MIN_MODEM),
   (band_mode si SOC 5G)] → set_config(ssid, password, threshold OPEN/rssi −127, bssid_set false,
   channel 0 + ALL_CHANNEL_SCAN, listen_interval 0, pmf capable true/required false) → connect ;
   plus set_storage(RAM) et set_max_tx_power si configuré. Jamais set_country / set_protocol /
   set_bandwidth / set_scan_parameters / set_event_mask.
2. **C6 = 2,4 GHz uniquement** (pas dual-band). ESPHome P4 n'appelle pas `esp_wifi_set_band_mode`.
   Si kayrozen n'émet qu'en 5 GHz, c'est physiquement mort (et ce serait du 201, pas du 203).
3. **Non proxifiées** : CSI, promiscuous, vendor IE, FTM, 80211_tx, action_tx, ROC, event_mask,
   get_home_channel, set_dynamic_cs, rssi_threshold, statis_dump, config_11b/80211_rate,
   connectionless wake, force_wakeup, get_tsf_time → weak `ESP_ERR_NOT_SUPPORTED` avec log
   `esp_wifi_remote_weak: ... unsupported`. Rien dans le chemin connect/scan/config n'est touché ;
   ni ESPHome ni nous n'en appelons dans ce chemin.
4. **Reason 203** : ESPHome le traite comme une erreur générique → retry_connect (2 essais/BSSID,
   re-priorisation, re-scan, restart adaptateur, timeout 46 s/attempt), sans jamais modifier la
   config entre essais. Notre watchdog 10 s est plus simple ; l'écart à corriger est dans la
   `wifi_config_t` (scan_method ALL_CHANNEL_SCAN, storage RAM, ordre start/config), pas dans la
   logique de retry.
