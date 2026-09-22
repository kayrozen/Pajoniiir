# Support HOST sur le port USB FS (OTG_FS / OTG1.1) de l'ESP32-P4 — rapport de recherche

Date : 2026-09-22. Sources vérifiées via GitHub API (issues/PRs/CHANGELOG), docs Espressif.

## 1. Oui, ça existe — et c'est récent

- **Le host sur le port FS (OTG1.1, GPIO26/27, FSLS PHY interne) fonctionne** avec `usb_host_config_t::peripheral_map = BIT(1)`. Preuve directe : issue espressif/esp-usb **#396** « esp_modem_usb_dte on ESP32-P4: how to properly initialize FS PHY2 (IEC-485) » (fév. 2026) — https://github.com/espressif/esp-usb/issues/396 — le reporter confirme : *« Setting `peripheral_map = BIT1` worked, problem solved. »*
- C'est aussi documenté dans l'**ESP-FAQ** : « How to use USB FS PHY as a host with ESP32-P4 » — https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/usb.html (conseil : `peripheral_map = BIT1` dans `usb_host_install`).
- **Le dual host simultané (HS + FS) n'est supporté que depuis esp-usb 1.3.0** (release 2026-03-04) via PR espressif/esp-usb **#402** « feat(usb/host): Add support for USB dual host on ESP32-P4 » (mergée 2026-02-19, commit `5d6ace9`) — https://github.com/espressif/esp-usb/pull/402. Il **clos** l'issue espressif/esp-idf **#18098** « ESP32P4 如何将USB-HS和USB-FS同时配置为Host » — https://github.com/espressif/esp-idf/issues/18098, où tore-espressif confirmait qu'avant, *seul un contrôleur pouvait être host à la fois* et que la 2e `usb_host_install()` échouait.
- CHANGELOG `host/usb/CHANGELOG.md` : `## [1.3.0] - 2026-03-04 — Added: USB Dual Host support on ESP32-P4`. Votre version **espressif/usb 1.5.0** (2026-06-16, nécessite IDF ≥ 5.5.3) contient donc bien le support.

## 2. Point crucial pour votre cas d'usage : API du dual host

Le dual host **ne se fait PAS avec deux `usb_host_install()` séparés**. Le PR #402 a restructuré la lib : une **seule** `usb_host_install()` avec `peripheral_map = BIT0 | BIT1` installe les PHY des deux ports, USBH/Enum/Hub gèrent les deux root ports (`phy_handles[HCD_NUM_PORTS]`). Détails dans le diff du PR :

- `usb_host_config_t::peripheral_map = 0` → défaut `BIT0` (rétrocompat).
- `fifo_settings_custom` est **ignoré** en multi-port (FIFO par défaut).
- `intr_flags` s'applique aux deux ports.
- La PHY est choisie automatiquement par port : UTMI pour l'index 0 (HS), PHY interne FSLS pour l'index 1 (FS).

### Votre architecture TinyUSB(HS) + Host Library(FS) — risque principal
Le dual host #402 est implémenté **à l'intérieur de la Host Library** (un seul USBH/driver pour les 2 ports). Faire tourner **esp_tinyusb en host sur HS en parallèle de la Host Library sur FS** est une combinaison différente : TinyUSB a son propre driver DWC2/PHY. La doc ne documente pas cette combinaison, et la phrase d'avant-fix (issue #18098, « only one can operate as a USB Host at a time ») s'appliquait au stack Espressif. Le fil esp32.com « ESP32-P4 and USB Device + Host » (t=46136, https://esp32.com/viewtopic.php?t=46136 — derrière un bot-check) traite device+host mais côté TinyUSB device. **Je n'ai trouvé aucun exemple officiel ni test prouvant TinyUSB-host(HS) + usb_host_lib(FS) simultanés** ; c'est la piste n°1 à suspecter si votre `usb_host_install(BIT(1))` s'installe mais n'énumère rien (conflit d'accès aux ressources USB partagées : PHY driver, PLL/clock 48 MHz, ISR).

### Symptôme « install OK, root port power ON, aucune énumération »
Cohérent avec un blocage du hub/enum driver, pas de la config du port. Pistes concrètes :
1. Si TinyUSB est actif : tester **sans** TinyUSB, `usb_host_install` seul avec `peripheral_map = BIT(1)` (le cas validé dans #396). Si ça énumère, le problème est la cohabitation TinyUSB/Host-Lib, pas le port FS.
2. Si vous vouliez les deux ports en host via la Host Library : **un seul** `usb_host_install` avec `peripheral_map = BIT0 | BIT1` (et abandonner TinyUSB, ou l'utiliser en device uniquement).
3. `skip_phy_setup=true` + `usb_new_phy(USB_PHY_TARGET_INT, USB_OTG_MODE_HOST)` manuel a échoué dans #396 tant que `peripheral_map` ne sélectionnait pas le contrôleur FS — laisser `skip_phy_setup=false` (défaut) gère la FSLS PHY.
4. Câblage : FS_PHY2 = D+ GPIO27, D− GPIO26 (FS_PHY1 GPIO24/25 est câblé au USB-Serial-JTAG par défaut ; swappable via efuse `USB_PHY_SEL` — https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_overview/usb_overview.html). Vérifier aussi l'alim VBUS de la clé.
5. Logs : activer plutôt les logs HCD (pas seulement USBH/HUB) et vérifier l'événement `HCD_PORT_EVENT_CONNECTION` ; DEBUG USBH sans rien = souvent pas d'interrupt du tout (PHY/clock ou conflit ISR avec TinyUSB).

## 3. Exemples/tests officiels

- **Aucun exemple démo dédié « host sur port FS du P4 »** dans esp-usb ou esp-idf (les exemples `msc_host`, `cdc_acm_host` restent sur le port HS par défaut). Le seul code officiel exerçant `peripheral_map = BIT0|BIT1` est le **test unitaire** `host/usb/test/host_test/usb_host_layer_test/main/usb_host_install_unit_test.cpp` (scénario « USB Dual Host install », PR #402) + tests cible `host/usb/test/target_test/usb_host/`.
- Référence applicative la plus proche : esp_modem `esp_modem_usb_dte` sur P4 (issue #396, résolue avec BIT1).

## 4. Versions

| Version espressif/usb | Dual host P4 | Host port FS (BIT1 seul) |
|---|---|---|
| ≤ 1.2.0 (2026-02-05) | ❌ non (2e install échoue) | ✅ (validé #396) |
| 1.3.0 (2026-03-04) → 1.5.0 (2026-06-16) | ✅ | ✅ |

IDF 6.0.2 + managed `espressif/usb 1.5.0` = version adequate. Attention : le composant exige IDF ≥ 5.5.3 (OK).

## 5. Conclusion

- Un device FS **s'énumère** sur le port OTG_FS du P4 avec la Host Library : `peripheral_map = BIT(1)`, PHY auto (`skip_phy_setup=false`), rien d'autre de spécial (pas de clock 48 MHz ni `otg_io_conf` à régler manuellement d'après les sources trouvées).
- Votre symptôme pointe très probablement vers la **cohabitation TinyUSB(HS) + Host Library(FS)** — combinaison non documentée, sans exemple officiel — plutôt que vers le port FS lui-même. Recommandation de test : `usb_host_install(BIT(1))` seul d'abord ; si OK, soit passer les deux ports sous la Host Library (`BIT0|BIT1`, un seul install), soit ouvrir une issue espressif sur la combinaison TinyUSB+HostLib.
