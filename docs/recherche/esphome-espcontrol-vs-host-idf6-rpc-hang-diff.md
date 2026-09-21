# Diagnostic différentiel : host P4 (IDF 6.0.2 + esp_hosted 2.12.13) hang sur les RPC Wi-Fi vs EspControl (ESPHome, host 2.12.12) qui connecte avec le même C6

**Date :** 2026-09-20 · **Statut : recherche uniquement — aucun patch appliqué au firmware.**
**Complément de :** [`esp-hosted-p4-c6-sdio-dropping-packets-rpc-hang.md`](esp-hosted-p4-c6-sdio-dropping-packets-rpc-hang.md) (recensement des cas, §signification des logs).

---

## 1. Question 1 — Quel backend `esp_wifi_remote` est actif chez nous ?

### 1.1 Réponse : `esp_hosted` (SDIO), pas `wifi_remote_over_eppp`

Trois preuves indépendantes :

1. **Nos logs montrent le tag `H_SDIO_DRV`** (`Dropping packet(s) from stream`, `Failed to push data to rx queue`). Ce tag n'existe que dans le driver SDIO d'esp_hosted (`host/drivers/transport/sdio/sdio_drv.c`, tag `H_SDIO_DRV`, lignes 1014/1082 sur v2.12.13 — vérifié dans la source). Le backend EPPP loguerait via `eppp_link`/`wifi_remote_over_eppp`, jamais via `H_SDIO_DRV`.
2. **Le Kconfig d'esp_wifi_remote choisit ESP-HOSTED par défaut** : `choice ESP_WIFI_REMOTE_LIBRARY / default ESP_WIFI_REMOTE_LIBRARY_HOSTED` — source : [`components/esp_wifi_remote/Kconfig.rpc.in` (esp-wifi-remote, branche main)](https://github.com/espressif/esp-wifi-remote/blob/main/components/esp_wifi_remote/Kconfig.rpc.in). Notre `sdkconfig.defaults` ne surcharge pas ce symbole → défaut actif.
3. **`wifi_remote_over_eppp` (0.3.3) n'est qu'une dépendance `require: private` d'esp_wifi_remote** (visible dans notre `dependencies.lock`) : le composant est téléchargé pour satisfaire la résolution, mais il n'est **pas sélectionné** tant que `CONFIG_ESP_WIFI_REMOTE_LIBRARY` vaut `HOSTED`. Sa présence dans `managed_components` n'intercepte rien. Doc backend : [page composant esp_wifi_remote](https://components.espressif.com/component/espressif/esp_wifi_remote) et [blog Espressif « esp-wifi-remote »](https://developer.espressif.com/blog/2025/09/esp-wifi-remote/) (menuconfig : `Component config → Wi-Fi Remote → Choose WiFi-remote implementation`, `ESP-HOSTED` par défaut, ~50 Mb/s vs ~20 Mb/s EPPP).

### 1.2 Comment vérifier / forcer (IDF 6.0.2)

```bash
# Après un build, dans build/config/sdkconfig.h :
grep -E "ESP_WIFI_REMOTE_LIBRARY_(HOSTED|EPPP|CUSTOM)" build/config/sdkconfig.h
# Attendu : CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED=1, les autres non définis.

# Forcer (si un jour doute) — sdkconfig.defaults :
CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED=y
```

⚠️ Le menu dépend d'IDF : `Kconfig.rpc.in` est sourcé par `Kconfig` d'esp_wifi_remote via `orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"` — IDF 6.0 est couvert (`Kconfig.idf_v6.0.in` existe dans le dépôt).

⚠️ Symbole à vérifier en parallèle sous IDF 6 (host P4) : `CONFIG_ESP_HOST_WIFI_ENABLED` doit être **désactivé**. Le laisser actif avec wifi-remote provoque « `net80211: OS adapter function version error! Version 8 is expected, but it is 0` » et un init Wi-Fi en échec — issue [#181 (EHM-203)](https://github.com/espressif/esp-hosted-mcu/issues/181), précisément sur P4 + IDF 6.0.x + esp_hosted 2.12.3.

---

## 2. Question 2 — La config hôte d'ESPHome (composant `esp32_hosted`), vérifiée dans les sources

Sources primaires : [`esphome/components/esp32_hosted/__init__.py` (dev)](https://github.com/esphome/esphome/blob/dev/esphome/components/esp32_hosted/__init__.py), [`esphome/components/esp32/__init__.py` (dev)](https://github.com/esphome/esphome/blob/dev/esphome/components/esp32/__init__.py), [doc esphome.io `esp32_hosted`](https://esphome.io/components/esp32_hosted/), [`wifi_component_esp_idf.cpp` (dev)](https://github.com/esphome/esphome/blob/dev/esphome/components/wifi/wifi_component_esp_idf.cpp), [`esphome/esp-hosted-firmware`](https://github.com/esphome/esp-hosted-firmware).

### 2.1 Versions épinglées par le composant (to_code, branche dev)

| Composant | Version épinglée ESPHome | Chez nous (contexte / lock commité) |
|---|---|---|
| `espressif/esp_hosted` | **2.12.12** (exact) | 2.12.13 (2.12.0 essayé) ; `dependencies.lock` commité = **2.12.11** → à vérifier, voir V0 |
| `espressif/esp_wifi_remote` | **1.6.3** (exact) | 1.6.4 (contexte) / 1.6.3 (lock) |
| `espressif/wifi_remote_over_eppp` | 0.3.3 | 0.3.3 (idem) |
| `espressif/eppp_link` | 1.1.5 | 1.1.5 (idem) |
| ESP-IDF | ≥ 5.3 exigé à la validation ; **recommandé 5.5.5** (`ESP_IDF_FRAMEWORK_VERSION_LOOKUP`, Arduino 3.3.11 → IDF 5.5.5) | **6.0.2** |

### 2.2 sdkconfig généré par le composant (transport SDIO)

| Option | Valeur ESPHome | Chez nous |
|---|---|---|
| `CONFIG_ESP_HOSTED_SDIO_SLOT_<n>` | slot (défaut 1) | slot 1 ✓ |
| `CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS` | défaut 4-bit | ✓ |
| `CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_{CLK,CMD,D0,D1_4BIT,D2_4BIT,D3_4BIT}_SLOT_1` | pins du YAML | via `CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD=y` (défauts EV board : CLK18/CMD19/D0-D3=14-17 — identiques à notre câblage) |
| `CONFIG_ESP_HOSTED_CUSTOM_SDIO_PINS` | `y` dès que des pins sont fournis | **non** (nous utilisons les défauts board) |
| `CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ` | défaut YAML **40 MHz** (400 kHz–50 MHz, « try reducing if loss of communication or reboots ») | 40 MHz (défaut Kconfig, range max P4 = 40 000) ✓ |
| `CONFIG_ESP_HOSTED_{SDIO}_RESET_ACTIVE_LOW/HIGH` | selon `active_high:` (obligatoire) | active-low ✓ (GPIO54) |
| `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE` | reset_pin du YAML | 54 ✓ |
| `CONFIG_SLAVE_IDF_TARGET_ESP32C6` | variant du YAML | ✓ |
| `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` | **NON positionné** (option `use_psram`, défaut **false**) | **`=y` chez nous** ← différence majeure |
| `CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER` | `y` sur host **P4** (shim ESP-NOW) | non positionné (sans objet : overlay inactif côté hôte, l'overlay C6 n'ajoute que des messages CustomRpc ignorés) |
| `CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS` | 8 (P4) | idem |
| RX SDIO | non touché → défaut Kconfig **`ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE`** | streaming (défaut) ✓ |
| Post-build | script `esp32_hosted.py.script` : link **whole-archive** de `libespressif__esp_hosted.a` (workaround symboles faibles) | non reproduit chez nous (probablement sans rapport : nos symboles sont résolus) |

### 2.3 Config Wi-Fi hôte (`wifi_component_esp_idf.cpp`)

- `scan_method` : `WIFI_FAST_SCAN` ou `WIFI_ALL_CHANNEL_SCAN` selon YAML ; `threshold.authmode` = OPEN/WPA_PSK/WPA2_PSK/WPA3_PSK selon mot de passe ; `listen_interval = 0` ;
- **`pmf_cfg` : `capable = true`, `required = false`** ;
- `esp_wifi_set_ps()` après start : `WIFI_PS_MIN_MODEM` / `MAX` / `NONE` selon YAML `power_save:` (défaut ESPHome = light → MIN_MODEM).

Notre `wifi_link` : `WIFI_PS_NONE` (défaut composant), boot en **SoftAP** (`WIFI_MODE_AP`, SSID Pajoniiir) puis switch STA ; scan/connect appelés depuis `wifi_link_worker`/`wifi_probe_task` (prio 4).

### 2.4 Firmware esclave ESPHome (notre `network_adapter_esp32c6.bin`)

Tag `v2.12.12` d'[esphome/esp-hosted-firmware](https://github.com/esphome/esp-hosted-firmware), build **IDF 5.5.5**, flash mode DIO, overlay CustomRpc ESP-NOW (canal « peer data transfer », inactif si l'hôte n'active pas `ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER`). EspControl utilise **hôte 2.12.12 + IDF 5.5.5 + esclave 2.12.12** → les trois combinaisons que nous avons essayées (2.12.0, 2.12.11, 2.12.13) étaient **toutes** désalignées avec l'esclave ; le combo exact d'EspControl (2.12.12/2.12.12) n'a jamais été testé chez nous.

---

## 3. Question 3 — Bugs connus expliquant « RPC sans réponse » + drops, et ce qui diffère d'EspControl

### 3.1 Mécanique du hang RPC (vérifié dans la source v2.12.13)

- Un RPC synchrone **a un timeout de 5 s** : `DEFAULT_RPC_RSP_TIMEOUT = 5` ([`host/drivers/rpc/slaveif/rpc_slave_if.h`](https://github.com/espressif/esp-hosted-mcu/blob/main/host/drivers/rpc/slaveif/rpc_slave_if.h)), utilisé par `wait_for_sync_response()` (`rpc_core.c`) et `rpc_wrap.c` (`new_req->rsp_timeout_sec = DEFAULT_RPC_RSP_TIMEOUT`). En timeout : `W « rpc_core: Timeout waiting for Resp for [0x…] »` puis retour en échec.
- Donc un `esp_wifi_scan_start()`/`esp_wifi_connect()` qui **ne retourne jamais** n'est pas bloqué dans l'attente de réponse : il est bloqué **en amont**, dans la chaîne TX : sémaphore `rpc_tx_sem` pris en `HOSTED_BLOCKING` (rpc_tx_thread, `rpc_core.c:557`), file `rpc_tx_q` poussée en `HOSTED_BLOCK_MAX`, ou envoi transport (`transport_pserial_send` → file TX SDIO). Si le thread rpc_tx est coincé (TX SDIO bloqué, réponse précédente jamais consommée), la file se remplit et l'appelant dort indéfiniment.
- Les deux messages vus à 2,5 s viennent du chemin **RX streaming** : `Dropping packet(s) from stream` = `is_valid_sdio_rx_packet()` invalide sur un paquet du flux → **tout le reste du stream est jeté** (les réponses RPC qui y circulent sont perdues) ; `Failed to push data to rx queue` = retour `ESP_FAIL` de `sdio_push_data_to_queue` dans `sdio_data_to_rx_buf_task` (lignes 1014 et 1082 de `sdio_drv.c` v2.12.13).

### 3.2 Bugs upstream de la même famille (détails dans le recensement lié)

| Issue | Ce qu'elle dit pour nous |
|---|---|
| [#243 (2.12.13, P4+C6)](https://github.com/espressif/esp-hosted-mcu/issues/243) | RX alloc échoue une fois → le retry ajouté en 2.12.12 ne court jamais (`NEW_PACKET` déjà effacé) → **RX host mort définitivement, RPC tous en timeout**, `h2s out` continue de compter. Signature observable : `W H_SDIO_DRV: RX buffer alloc failed (len=…)` (WARN, une seule fois), compteur `s2h in` gelé. Workaround confirmé : `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y` + `CONFIG_CACHE_L2_CACHE_LINE_64B=y`. |
| [#219 (EHM-250)](https://github.com/espressif/esp-hosted-mcu/issues/219) | `MEMPOOL_PREFER_SPIRAM=y` + **ligne de cache L2 = 128 B** → bloc mempool 1600 B : `1600 % 128 = 64` → **une moitié des TX rejetée en DMA** (`Failed to send data: 258`). N'arrive que si L2=128 B. **IDF 6.0.2, P4 : défaut L2 = 128 KB/64 B** (`Kconfig.cache` : `default CACHE_L2_CACHE_LINE_64B if CACHE_L2_CACHE_128KB/256KB`, 128 B seulement si L2 512 KB) → sain par défaut, mais à vérifier dans notre sdkconfig. |
| [#199 (EHM-221)](https://github.com/espressif/esp-hosted-mcu/issues/199) | `Dropping packet` + `Failed to push data to rx queue` dès `esp_hosted_connect_to_slave()` avec **`MEMPOOL_PREFER_SPIRAM` + flash encryption**. |
| [#221 (EHM-253)](https://github.com/espressif/esp-hosted-mcu/issues/221) | Surcharge uplink : tâche Wi-Fi **esclave** wedgée (`xQueueSend portMAX_DELAY`), stall RX host silencieux ; drain à limiter par le registre longueur et non le bit `NEW_PACKET`. |
| [#220 (EHM-252)](https://github.com/espressif/esp-hosted-mcu/issues/220) / fix 2.12.12 | lecture bus all-ones déclarée succès → lien mort non détecté. Fix dans **2.12.12** (hôte). Notre esclave 2.12.12 l'a côté hôte... mais c'est notre *hôte* qui doit l'avoir (2.13 l'a, 2.12.11/2.12.0 non). |
| [#210 (EHM-235)](https://github.com/espressif/esp-hosted-mcu/issues/210) | packet mode : esclave cesse de répondre aux RPC après 30–80 s (`Timeout waiting for Resp 0x101`) ; cause = esp_wifi_remote < 1.3.1 avec esp_hosted ≥ 2.11.0 (double-free payload RX) + corruption heap (fix 2.12.10). Nous sommes 1.6.3/1.6.4 → non concernés. |
| Changelog 2.12.13 | « fixed **RPC serial reassembly stalling the RX datapath** when an RPC handler is slow », « fixed RPC response parse failures being reported as success », mutex sur les tables de réponses sync/async : les chemins de notre symptôme ont été **activement remaniés entre 2.12.12 et 2.12.13**. |
| [Changelog 2.12.9](https://components.espressif.com/components/espressif/esp_hosted/versions/2.12.13/changelog) | « fix build break for IDF v6 when enable power save (API renamed) » → le support IDF 6 est **partiel et récent**. |
| CI esp_hosted v2.12.13 (`.gitlab/ci/regression_pipeline_jobs.yml`) | matrice hôte **v5.3.x–v5.5.x uniquement** ; commentaire : « IDF master and v6.x don't build mqtt example with wifi-remote and ESP-Hosted ». **IDF 6.0.2 n'est pas testé en CI upstream** → combinaison inconnue côté Espressif. |

### 3.3 Ce qui diffère d'EspControl — classé par probabilité

| # | Différence | Probabilité | Justification |
|---|---|---|---|
| 1 | **`CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y` chez nous ; ESPHome le laisse OFF** (`use_psram` défaut false) | **Haute** | Seule grande divergence sdkconfig transport. Interagit avec #243 (RX alloc → stall permanent), #219 (si ligne L2 128 B), #199. Notre app consomme la PSRAM (LVGL, XIP `SPIRAM_FETCH_INSTRUCTIONS`, audio 200 MHz) → pression bandeau/heap inexistante chez ESPHome. |
| 2 | **Version hôte jamais alignée** : 2.12.0/2.12.11/2.12.13 essayés vs esclave 2.12.12 ; EspControl = 2.12.12 exact | **Haute** | Cas [A] (esphome#10956) : symptômes identiques (drops + assoc 203 + RPC sans réponse) résolus **uniquement** par l'alignement hôte/esclave. 2.12.13 remanie précisément les tables de réponses RPC (changelog). |
| 3 | **IDF 6.0.2** (EspControl : 5.5.5) — hors matrice CI upstream | **Moyenne-haute** | Changements sdmmc/driver esp_driver_sdmmc en IDF 6 non validés upstream (§3.2, CI) ; seule une rustine de build IDF v6 existe (2.12.9). |
| 4 | **Charge/usage différent** : boot en SoftAP puis switch STA, `WIFI_PS_NONE`, tâches audio 1 kHz + LVGL + PPA + USB → heap interne fragmenté | **Moyenne** | Le stall #243 se déclenche sous pression heap ; `WIFI_PS_NONE` multiplie le trafic SDIO vs MIN_MODEM ; ESPHome démarre en STA. Ne concerne pas EspControl (image légère, STA seul). |
| 5 | **Signal integrity / marges SDIO** | **Basse** (mais non nulle) | EspControl connecte avec le même C6 → le bus passe à 40 MHz en pratique. Mais « 20 MHz = boot loop » est **anormal** (baisser la fréquence ne doit pas casser le boot) → suspecte plutôt une interaction reset/timing (`RESET_ONLY_IF_NECESSARY`, délai 1500 ms) qu'un problème purement électrique. Les cas #167/#121 (0x107, tous matériels) restent la classe à exclure. |
| 6 | Overlay ESP-NOW CustomRpc du C6 vs hôte IDF standard | **Très basse** | Overlay purement additif sur un canal ignoré par un hôte qui n'active pas `PEER_DATA_TRANSFER` (aucun cas documenté d'incompatibilité). |

---

## 4. Vérifications à faire chez nous (classées, aucune modification de code métier)

- **V0 — Fixer l'état réel.** `dependencies.lock` commité = esp_hosted **2.12.11** / esp_wifi_remote 1.6.3, mais le contexte décrit des builds 2.12.13. Vérifier ce qui est réellement lié : `grep -E "esp_hosted|esp_wifi_remote" build/project_description.json` (ou sortie `idf.py build`) et régénérer un lock propre.
- **V1 — Logs signatures (WARN déjà visibles, passer `H_SDIO_DRV`, `rpc_core`, `transport` en INFO/DEBUG)** : chercher dans l'ordre : `Version mismatch: Host … Co-proc …` (handshake), `RX buffer alloc failed (len=…)` (#243), `mempool OOM`, `Failed to send data: 258` (#219), `Timeout waiting for Resp` (prouverait que le RPC timeout et que le hang est ailleurs), `SDIO mode: slave: streaming, host: streaming` (cohérence des deux côtés).
- **V2 — `CONFIG_ESP_HOSTED_PKT_STATS=y` (+ `ESP_HOSTED_PKT_STATS_INTERVAL_SEC=2`)** et regarder si `s2h in` gèle pendant que `h2s out ok` progresse → signature directe du stall RX #243.
- **V3 — Test différentiel n°1 (le plus décisif) : épingler le host au combo exact d'EspControl** : `espressif/esp_hosted==2.12.12` + `espressif/esp_wifi_remote==1.6.3` (toujours IDF 6.0.2 d'abord). Si ça connecte → problème spécifique 2.12.13/mismatch ; si ça hang → problème config/IDF.
- **V4 — Test différentiel n°2 : `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` non défini** (défaut ESPHome) ; si assert `sdio_mempool_create` au boot (RAM interne saturée), au moins documenter, puis alternative : garder PSRAM mais **forcer explicitement `CONFIG_CACHE_L2_CACHE_LINE_64B=y`** (le défaut IDF 6.0.2 P4 est déjà 64 B — le vérifier dans `build/config/sdkconfig.h`, #219/#243).
- **V5 — Vérifier `CONFIG_ESP_HOST_WIFI_ENABLED` non défini** (issue #181, IDF 6, host P4).
- **V6 — Localiser le blocage** : task WDT + backtrace du task `wifi_link` (et `rpc_tx` si dumpé) pour distinguer blocage sur `rpc_tx_q` (push bloquant) vs sémaphore RPC vs envoi transport. Les RPC sync timeout à 5 s : si l'appel hang plus de 5 s sans log `Timeout waiting for Resp`, le blocage est avant l'attente (§3.1).
- **V7 — Tests d'usage** : (a) démarrer directement en STA sans SoftAP (ordre ESPHome) ; (b) `WIFI_PS_MIN_MODEM` au lieu de `WIFI_PS_NONE` ; (c) scan immédiatement après boot vs après quelques secondes.
- **V8 — Filet de sécurité (recommandation du recensement §7.6)** : s'abonner aux events hostés `INIT`/`HEARTBEAT`/`TRANSPORT_FAILURE` (exemple `host_hosted_events`) plutôt qu'un watchdog artisanal.
- **V9 — Si V3+V4 échouent** : envisager host **2.12.12 + IDF 5.5.5** (réplique exacte d'EspControl) dans un build jetable pour isoler IDF 6 ; et/ou flasher un esclave **2.12.13** pour alignement montant.

---

## 5. Sources

- esp_wifi_remote backend : <https://github.com/espressif/esp-wifi-remote/blob/main/components/esp_wifi_remote/Kconfig.rpc.in> · <https://components.espressif.com/component/espressif/esp_wifi_remote> · <https://developer.espressif.com/blog/2025/09/esp-wifi-remote/>
- ESPHome `esp32_hosted` : <https://github.com/esphome/esphome/blob/dev/esphome/components/esp32_hosted/__init__.py> · <https://esphome.io/components/esp32_hosted/> · <https://github.com/esphome/esphome/blob/dev/esphome/components/esp32/esp32_bare.py> (versions IDF recommandées, 5.5.5) · <https://github.com/esphome/esphome/blob/dev/esphome/components/wifi/wifi_component_esp_idf.cpp>
- Firmware esclave : <https://github.com/esphome/esp-hosted-firmware> (tag v2.12.12)
- Source host v2.12.13 (tags GitHub) : `host/drivers/transport/sdio/sdio_drv.c` (lignes 946–1082 : drops stream), `host/drivers/rpc/core/rpc_core.c` (`rpc_tx_sem` ligne 557, `wait_for_sync_response`), `host/drivers/rpc/slaveif/rpc_slave_if.h` (`DEFAULT_RPC_RSP_TIMEOUT 5`), `host/port/esp/freertos/include/port_esp_hosted_host_config.h` (RX streaming défaut), `Kconfig` (RX streaming défaut y, 40 000 kHz)
- Issues : [#243](https://github.com/espressif/esp-hosted-mcu/issues/243) · [#219](https://github.com/espressif/esp-hosted-mcu/issues/219) · [#221](https://github.com/espressif/esp-hosted-mcu/issues/221) · [#220](https://github.com/espressif/esp-hosted-mcu/issues/220) · [#210](https://github.com/espressif/esp-hosted-mcu/issues/210) · [#199](https://github.com/espressif/esp-hosted-mcu/issues/199) · [#181](https://github.com/espressif/esp-hosted-mcu/issues/181) · [#167](https://github.com/espressif/esp-hosted-mcu/issues/167) · [esphome#10956](https://github.com/esphome/esphome/issues/10956)
- Changelog : <https://components.espressif.com/components/espressif/esp_hosted/versions/2.12.13/changelog> (2.12.12, 2.12.13 : fixes RPC/all-ones/mempool ; 2.12.9 : IDF v6 power save)
- CI upstream (IDF testés) : <https://github.com/espressif/esp-hosted-mcu/blob/v2.12.13/.gitlab/ci/regression_pipeline_jobs.yml> · `sanity_pipeline_jobs.yml`
- Cache L2 P4 (IDF 6.0.2) : <https://github.com/espressif/esp-idf/blob/v6.0.2/components/esp_system/port/soc/esp32p4/Kconfig.cache>
