# Arbitrage USB MSC vs TinyUSB host sur ESP32-P4 — cohabitation `usb_host_msc` (esp_usb) et TinyUSB host (DDJ MIDI/audio isochrone)

**Date :** 2026-09-21 · **Statut : recherche web + audit repo — aucun patch appliqué au firmware.**
**Complément de :** [`docs/migration/migration-jc1060-components.md`](../migration/migration-jc1060-components.md) (§Niveau 2, conflit `usb_storage` signalé).
**Environnement de référence :** IDF 6.0.2, main-deck-jc1060 (TinyUSB 0.21.0 vendored, DDJ-400 OK en v117), main-deck-p4 (`usb_storage` = `espressif/usb_host_msc` 1.2.0 + `espressif/usb` 1.5.0).

---

## TL;DR — réponse en 4 lignes

1. **Le P4 n'a pas UN contrôleur « DWC3 » mais DEUX contrôleurs USB 2.0 OTG** : un High-Speed (cœur DWC2 + PHY UTMI, pins dédiées `USB_DP`/`USB_DM`) et un Full-Speed (cœur OTG1.1 + PHY FSLS intégrée, pins muxées GPIO26/27 par défaut, GPIO24/25 en option). Matériellement, les deux peuvent être host en même temps — les pins ne se partagent pas entre HS et FS.
2. **MSC via TinyUSB (tuh_msc) existe et se combine bien avec MIDI/audio host**, mais exige hub + glue FatFS maison + budget endpoints, et la maturité des exemples host est moindre que la pile IDF.
3. **Pas de projet connu qui fait cohabiter les deux stacks host sur le même contrôleur** (impossible), ni de switch dynamique documenté de façon fiable. En revanche, le split **TinyUSB sur OTG_HS + pile IDF sur OTG_FS** est l'architecture logique, déjà validée dans l'autre sens (TinyUSB device sur OTG1.1 + pile IDF host sur OTG_HS simultanés, cf. esp32.com) — pour host+host il reste un POC hardware à faire.
4. **Recommandation Pajoniiir : plan A = `usb_storage` (pile IDF) sur le second port (OTG_FS)** via `peripheral_map = BIT1`, TinyUSB inchangé sur OTG_HS. Plan B si la board n'expose pas GPIO26/27 : MSC-via-TinyUSB derrière un hub. Switch dynamique : à écarter.

---

## 1. Question 1 — Un ou plusieurs ports/contrôleurs host sur l'ESP32-P4 ?

### 1.1 Réponse : deux contrôleurs OTG distincts, PHYs dédiées

Le préambule « single DWC3 controller » est à corriger : l'ESP32-P4 embarque **deux périphériques USB 2.0 OTG indépendants** (le cœur est un Synopsys **DWC2** 4.00a, `GSNPSID 0x4F54400A`, pas un DWC3 — vérifié dans `dwc2_info.md` du TinyUSB vendu) :

| | OTG_HS | OTG_FS (« OTG1.1 ») |
|---|---|---|
| Cœur | DWC2 HS, base `0x5000_0000` | DWC2 FS, base `0x5004_0000` |
| PHY | UTMI intégrée, **pins dédiées** `USB_DP`/`USB_DM` (pins 49/50, non muxées GPIO) | FSLS intégrée, **muxée** GPIO26/27 par défaut (GPIO24/25 en option) |
| Modes | HS 480 / FS 12 / LS 1.5 Mb/s, host & device | FS 12 Mb/s, host & device |
| Canaux host (DWC2 `ep_count`) | **16** (`ep_in_count` 8, DFIFO 1024) | 7 (`ep_in` 5, DFIFO 256) |

Sources :
- Datasheet ESP32-P4 (§3.4.20/3.4.21 + pinout) : le mux FS porte sur « GPIO24–GPIO25 et GPIO26–GPIO27 », FS OTG défaut = GPIO26/27, USB Serial/JTAG défaut = GPIO24/25 ; « The pins connected to D+ and D− signals for two pairs of USB PHY are multiplexed with GPIO24-GPIO25 and GPIO26-GPIO27 ». Le HS OTG a ses propres pads `USB_DP`/`USB_DM` — https://www.erlendervik.no/.div/ESP32_P4_Chip_Datasheet__EN.pdf et [Schematic Checklist P4](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32p4/schematic-checklist-esp32p4.html) : « If both functions are used simultaneously, GPIO24/GPIO25 and GPIO26/GPIO27 will serve as two separate pins for each function ».
- TinyUSB `dwc2_esp32.h` (vendored 0.21.0, `firmware/main-deck-jc1060/components/tinyusb/tinyusb_src/portable/synopsys/dwc2/dwc2_esp32.h`) : « Port0 to OTG_FS, and Port1 to OTG_HS » avec les deux entrées `_dwc2_controller[]` (FS 0x50040000, HS 0x50000000).
- [esp-usb, USB Host (P4)](https://docs.espressif.com/projects/esp-usb/en/latest/esp32p4/usb_host.html) : « ESP32-P4 has two USB 2.0 OTG controllers: one High-Speed and one Full-Speed. Each controller can operate as a USB host independently, so either one alone or both together can function as USB hosts simultaneously » (état *latest/master*).
- TinyUSB issue [#2791](https://github.com/hathach/tinyusb/issues/2791), tore-espressif : « ESP32-P4 has two USB peripherals, one FS and one HS, each with its dedicated PHY. So theoretically, one can work as Host and the other as Device (or both device, or both host). »

### 1.2 `usb_new_phy`, UTMI vs mux — ce que fait notre code

`usb_tu_app.c` (jc1060) initialise la PHY via l'API IDF `usb_new_phy()` (header `esp_private/usb_phy.h` en IDF 6) :

```c
usb_phy_config_t phy_cfg = { .target = USB_PHY_TARGET_UTMI, ... };  /* P4 OTG_HS = UTMI PHY */
usb_new_phy(&phy_cfg, &s_phy);
... tuh_init(1);   /* rhport 1 = OTG_HS */
```

- `USB_PHY_TARGET_UTMI` = PHY HS du P4 ; `USB_PHY_TARGET_INT` = transceiver FSLS interne (le mux FS_PHY1/FS_PHY2 — GPIO24/25 vs GPIO26/27 — est géré par la couche PHY/IOMUX). C'est exactement le « UTMI vs mux » de la question : les **deux** cibles existent sur le P4 et pilotent **deux contrôleurs différents**, pas le même.
- Conséquence importante : la PHY FS (GPIO26/27) reste **libre** quand TinyUSB tourne sur l'UTMI — rien ne se dispute les pins.

### 1.3 La limite logicielle IDF 6.0.x à connaître

- La pile host IDF (« Host Library ») est installable sur le périphérique de son choix via `usb_host_config_t::peripheral_map` (documenté au moins depuis IDF 5.3.4 pour le P4 : « peripheral_map = BIT1; installs USB host on peripheral 1 » — mapping des bits défini par `USB_DWC_LL_GET_HW()`). Sur cible HS-capable, défaut = HS.
- **Mais** en IDF 6.0.x, la doc [USB Host P4 (v6.0.3)](https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32p4/api-reference/peripherals/usb_host.html) précise : « due to a current software limitation, **only one can operate as a USB Host at a time**. Support for dual USB Host operation is planned for a future update » (le « both together » de la page esp-usb *latest* reflète master, pas 6.0.2). Les champs « dual host » de `usb_host_config_t` (fifo_settings_custom ignoré, intr_flags « for all ports ») visibles dans la doc latest confirment que ça arrive mais pas que c'est dans 6.0.2.
- **Pour Pajoniiir, cette limite n'est pas bloquante** : il ne faut qu'**une** instance de la pile IDF (sur OTG_FS pour le MSC), l'autre contrôleur étant piloté par TinyUSB, qui est une pile séparée hors du champ de cette limite. Le cas symétrique et documenté est [esp32.com, « ESP32-P4 and USB Device + Host » (t=46136)](https://esp32.com/viewtopic.php?t=46136) : TinyUSB device sur OTG1.1 (menuconfig « USB Peripheral = OTG1.1 ») **en même temps** que la pile IDF host sur OTG_HS. Host(IDF) + host(TinyUSB) sur les deux contrôleurs est le même découplage — non documenté tel quel d'où le POC hardware requis (§5).

### 1.4 Le cas JC1060P470C

- La carte expose plusieurs USB-C (CNX-software : « 3× USB Type-C ports » ; Spotpear : flash par « USB1 »). Le port qui fait marcher le DDJ est forcément câblé sur les pads dédiés HS (`USB_DP`/`USB_DM`).
- **À vérifier sur le schéma** (rar Spotpear, non ouvert ici) : un second connecteur est-il câblé sur **GPIO26/27** (FS OTG, chemin recommandé du plan A) ? Les interfaces listées (Type-C, GPIO headers) le laissent plausible mais ce n'est pas confirmé. NB : le FS ne délivre pas de négociation CC Type-C native — un connecteur Type-A (ou câble/adapter avec pull-downs CC côté host) est de toute façon le montage attendu pour un port host.
- Alimentation : VBUS des deux ports doit venir du rail 5 V de la carte ; budget DDJ-400 (~500 mA, bus-powered) + clé USB (~200 mA) → alimentation externe dimensionnée requise (Spotpear demande déjà >600 mA pour la carte seule).

---

## 2. Question 2 — MSC via TinyUSB en plus de MIDI/audio ?

### 2.1 Oui, le driver existe

- Le host stack TinyUSB inclut **MSC host** (classe `tuh_msc`) avec API `tuh_msc_mount_cb` / `tuh_msc_umount_cb` / `tuh_msc_read10`/`write10` (SCSI block-level) et un exemple complet avec FatFS : [`examples/host/msc_file_explorer`](https://github.com/hathach/tinyusb/blob/master/examples/host/msc_file_explorer/src/msc_app.c) (explorateur de fichiers sur clé USB, via FatFS). La FAQ TinyUSB confirme le support hub (« Yes, through a USB hub »).
- Notre TinyUSB vendu (0.21.0) contient `host/msc_host.c` + `class/msc/` ; il suffit de passer `CFG_TUH_MSC 1` dans `tusb_config.h` (actuellement `CFG_TUH_MSC 0`, `CFG_TUH_HUB 0`, `CFG_TUH_DEVICE_MAX 1`).

### 2.2 Config concrète à prévoir (plan B)

```c
#define CFG_TUH_MSC          1
#define CFG_TUH_HUB          1      /* un seul port physique : hub obligatoire */
#define CFG_TUH_DEVICE_MAX   3      /* hub + DDJ + clé */
#define CFG_TUH_MSC_MAX      1
#define CFG_TUH_ENDPOINT_MAX 16     /* défaut; HS a 16 canaux host, large */
```

Budget endpoints sur OTG_HS (16 canaux) : audio UAC1 DDJ ≈ 3 EP (iso in, iso out, feedback int) + MIDI 2 EP int + MSC 2 EP bulk + EP0 + hub 1 EP int ≈ **9/16** → passe confortablement. Le gros du travail n'est pas les endpoints mais la **glue FatFS** : implémenter les fonctions disque (`disk_read`/`disk_write`/`disk_ioctl`) sur `tuh_msc_read10/write10`, gestion mount/umount, et l'ordonnancement (transfers 64 ko, retries) — c'est ce que fait l'exemple `msc_file_explorer`, à porter. Le composant `usb_storage` actuel (glue FATFS + bus Rekordbox au-dessus de `usb_host_msc`) devrait pouvoir conserver sa couche haute et juste ré-implémenter sa couche « transport ».

### 2.3 Limites et risques documentés du stack tuh

- **Un seul instance de stack** : TinyUSB n'a pas de notion d'instance multi-contrôleur simultanée (variables statiques partagées, cf. espressif/esp-idf [#15810](https://github.com/espressif/esp-idf/issues/15810), commentaire roma-jam : « there is only one internal static variable `_usbd_rhport`... », feature request upstream [#3092](https://github.com/hathach/tinyusb/issues/3092)). Pour nous : pas gênant (une seule instance sur rhport 1), mais ça ferme l'option « TinyUSB sur les deux contrôleurs ».
- **Isochrone host** : l'exemple [`host/audio_host`](https://docs.tinyusb.org/en/latest/examples/host/audio_host.html) (UAC2) fonctionne mais est explicitement décrit avec « Limitations and trade-offs » (feedback 10.14/16.16, pas de gestion fine des alt-settings). Notre chemin UAC1 DDJ marche déjà en v117 — ne pas y toucher. Ajouter MSC bulk sur le même bus ne dégrade pas l'iso par construction (réservation de bande passante différente), mais **à re-valider à l'oreille** : nos historiques de glitches audio (bisect v52-v54, DMA2D) montrent que le chemin iso est sensible à la charge CPU/DSI ; un driver MSC qui boufferait du CPU (lecture FATFS dans le mauvais task) ferait réapparaître des xruns. Prévoir les lectures MSC dans un task dédié basse priorité, jamais dans le callback tuh.
- **Composite / énumération** : l'énumération multi-interfaces est OK, mais des devices réels à classes composites posent encore des soucis — ex. tinyusb [#3801](https://github.com/hathach/tinyusb/discussions/3801) : `tuh_midi_mount_cb` jamais appelé pour un device UAC2+MIDI composite (Zoom G5n). Le DDJ-400 (UAC1 + MIDI) passe ; une clé USB derrière un hub est un device distinct, risque plus faible, mais prévoir un test avec plusieurs clés réelles (exFAT non géré par FatFS — rester FAT32, ce que fait déjà la couche Rekordbox).
- **Hub** : `CFG_TUH_HUB` du TinyUSB upstream est fonctionnel mais peu éprouvé sur ESP32-P4 (issues #2943/#3170 montrent des soucis électriques/DWC2-S3 résiduels). Ajouter un hub = un point de défaillance de plus + budget courant.
- Firmware size/RAM : MSC + FatFS glue ≈ quelques ko ; buffer stream audio 32 ko déjà dimensionné.

---

## 3. Question 3 — Projets connus : cohabitation des deux stacks ou switch dynamique ?

- **Même contrôleur, deux stacks host : impossible** par construction (une seule pile peut être `hcd_init` sur un contrôleur ; les deux piles installeraient leur ISR/regs sur les mêmes regs DWC2). Aucun projet sérieux ne le fait ; les threads esp32.com concluent tous « une pile par contrôleur ».
- **Deux contrôleurs, deux piles (le sens Device TinyUSB + Host IDF)** : documenté et confirmé fonctionnel sur P4 — esp32.com t=46136 : « go in the config of ESP-IDF and make sure to select OTG1.1 as the USB Peripheral for TinyUSB. [Le HS et le FS] can be used at the same time (HS, FS, ...) ». C'est le meilleur précédent pour notre direction (Host IDF sur FS + TinyUSB sur HS), mais je n'ai **pas trouvé de projet public qui le fait en host+host** — d'où le statut « POC à valider » du plan A.
- **Dual host 100 % IDF (les deux contrôleurs sur la Host Library)** : supporté sur master esp-usb (« either one alone or both together... simultaneously ») mais **pas encore en IDF 6.0.x** (« only one can operate as a USB Host at a time », doc v6.0.3). À re-vérifier en IDF 6.1/7 — ça deviendrait alors une alternative propre (les DEUX stacks disparaissent au profit d'une seule), mais exigerait de migrer le chemin DDJ MIDI/audio vers la pile IDF, ce qui n'existe pas (pas de classe audio isochrone IDF) → hors sujet pour nous.
- **Switch dynamique (MSC au boot puis bascule TinyUSB)** : pas de projet connu fiable. `usb_host_uninstall()` existe (API IDF, nécessite tous clients déregistrés + devices freed), et TinyUSB expose `tuh_deinit`/`tuh_init` en 0.21 ; mais la séquence implique de couper la VBUS/ressetter le bus (le DDJ-400 perdrait son énumération en plein set), de re-séquencer les PHYs, et gérer les courses. Pour un device live DJ où le MSC (bibliothèque) et le DDJ (contrôle+audio) sont nécessaires **simultanément**, ce pattern est intrinsèquement inadapté. À écarter.

---

## 4. Recommandation pour Pajoniiir

### 4.1 Arbre de décision

```
Le JC1060P470C a-t-il un connecteur câblé sur GPIO26/27 (FS OTG) ?
│
├─ OUI → PLAN A (recommandé) : deux contrôleurs, deux piles
│         TinyUSB host (DDJ MIDI+audio iso) sur OTG_HS [inchangé]
│         usb_storage (pile IDF usb_host_msc) sur OTG_FS, peripheral_map = BIT1
│
└─ NON → PLAN B : MSC-via-TinyUSB sur OTG_HS derrière un hub
          CFG_TUH_MSC/HUB=1 + glue FatFS (port msc_file_explorer)
          (Plan C = hardware : repin/patch board sur GPIO26/27, ou hub alimenté
           + plan B, à arbitrer avec l'effort plan B)
```

### 4.2 Plan A — détails d'implémentation (estimation : 3–5 jours + test hardware)

1. **Firmware** (petit diff sur `usb_storage.c`) :
   ```c
   const usb_host_config_t host_cfg = {
       .peripheral_map = BIT1,     /* OTG_FS au lieu du HS par défaut */
       .skip_phy_setup = false,    /* IDF configure la FSLS PHY (GPIO26/27) */
       ...
   };
   ```
   La couche USB_PID/VID/SCSI/FATFS de `usb_storage` (Rekordbox) est indépendante de la vitesse ; juste la confirmation que le bus FS (12 Mb/s, ~1 Mo/s effectif) suffit : streaming 2 decks FLAC 44.1/16 ≈ 0,35 Mo/s → OK avec marge ; les scans/bibliothèques seront ~5× plus lents qu'en HS (non bloquant si indexé au repos).
2. **Aucune modif TinyUSB** — le DDJ n'a pas de raison de bouger (le risque principal du sujet est donc évité).
3. **Vérifications avant d'engager** :
   - Schéma JC1060 : GPIO26/27 routés vers un connecteur ? (bloquant pour A)
   - `peripheral_map` bien présent dans IDF 6.0.2 (`usb_host.h` du v6.0.2 ; présent ≥ 5.3.x) et comportement FS host : MSC bulk-only + `usb_host_lib_set_root_port_power` fonctionnel sur OTG_FS.
   - POC 1 journée : build jc1060 + `usb_storage` rebranché sur FS, monter une clé FAT32, lire un fichier pendant que le DDJ streame — critère d'acceptation : zéro xrun audio (comparer aux compteurs `s_pb_cb_count` existants), mount FATFS OK.
4. **Risques résiduels** : (a) la limitation IDF « un seul host à la fois » ne concerne que deux instances IDF — à confirmer par le POC qu'IDF-FS + TinyUSB-HS coexistent sans conflit d'ISR/PHY (aucun signal contraire trouvé, precedent t=46136 dans l'autre sens) ; (b) alimentation 2 ports ; (c) si la Host Library IDF refusait statiquement le FS sur 6.0.2, fallback immédiat = plan B.

### 4.3 Plan B — MSC-via-TinyUSB (estimation : 1,5–2 semaines)

`CFG_TUH_MSC=1` + hub + `CFG_TUH_DEVICE_MAX=3` + glue FatFS portée de `msc_file_explorer` + refactor de la couche transport de `usb_storage`. Risques : maturité hub tuh, énumérations composites/hub réelles, budget VBUS, et zone de contact avec le chemin audio iso (à re-écouter après chaque changement). Avantage : un seul port physique, pas de dépendance au schéma de la carte.

### 4.4 Ce qu'il ne faut PAS faire

- Lancer `usb_host_msc` (pile IDF, défaut = OTG_HS) **en même temps** que TinyUSB sur le même contrôleur — c'est le conflit actuel signalé dans la doc de migration ; les deux pilotes s'écraseraient sur les regs DWC2/ISR de l'OTG_HS.
- Un switch dynamique (§3) : perte d'énumération du DDJ en live, séquence PHY fragile, aucun précédent solide.
- Migrer le chemin audio DDJ hors TinyUSB : la pile IDF n'a pas de classe audio isochrone ; le chemin v117 est le seul qui marche.

---

## 5. Références

- [esp-usb — USB Host (ESP32-P4, latest)](https://docs.espressif.com/projects/esp-usb/en/latest/esp32p4/usb_host.html) (2 contrôleurs hostables, `peripheral_map`, hub, limitations)
- [ESP-IDF v6.0.3 — USB Host P4](https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32p4/api-reference/peripherals/usb_host.html) (limite « un seul host IDF à la fois » en 6.0.x)
- [ESP32-P4 Datasheet](https://www.erlendervik.no/.div/ESP32_P4_Chip_Datasheet__EN.pdf) / [Schematic Checklist P4](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32p4/schematic-checklist-esp32p4.html) (pins USB : HS dédiées, FS mux GPIO24/25–26/27)
- [IDF v5.3.4 — USB Host P4 (peripheral_map=BIT1)](https://docs.espressif.com/projects/esp-idf/zh_CN/v5.3.4/esp32p4/api-reference/peripherals/usb_host.html)
- [esp-idf#15810](https://github.com/espressif/esp-idf/issues/15810) + [tinyusb#3092](https://github.com/hathach/tinyusb/issues/3092) (une seule instance TinyUSB)
- [tinyusb#2791](https://github.com/hathach/tinyusb/issues/2791) (P4 HS/FS, PHYs dédiées) · [#3108](https://github.com/hathach/tinyusb/issues/3108) (host P4 supporté, split transactions DWC2)
- [tinyusb msc_file_explorer](https://github.com/hathach/tinyusb/blob/master/examples/host/msc_file_explorer/src/msc_app.c) · [audio_host limitations](https://docs.tinyusb.org/en/latest/examples/host/audio_host.html) · [FAQ hub](https://docs.tinyusb.org/en/latest/faq.html)
- [esp32.com t=46136](https://esp32.com/viewtopic.php?t=46136) (TinyUSB OTG1.1 + host IDF simultanés) · [#3801](https://github.com/hathach/tinyusb/discussions/3801) (composite UAC2+MIDI)
- Repo : `firmware/main-deck-jc1060/main/usb_tu_app.c` (usb_new_phy UTMI, rhport 1), `components/tinyusb/tinyusb_src/tusb_config.h` (CFG_TUH_*), `portable/synopsys/dwc2/dwc2_esp32.h` (2 contrôleurs, 16/7 EP), `firmware/main-deck-p4/components/usb_storage/` (usb_host_msc 1.2.0).

*Vérifs non couvertes ici : schéma JC1060 (rar) pour GPIO26/27 ; existence exacte de `peripheral_map` dans le header usb_host.h d'IDF 6.0.2 installé (à faire au premier build POC) ; test hardware FS host.*

## Vérification schéma JC1060P470C (2026-09-21, post-rapport)

`/tmp/schem.txt` (extraction du PDF vendor) : **GPIO26 = TX1, GPIO27 = RX1**
— le mux OTG_FS est routé en UART1 externe sur la board, PAS vers un
connecteur USB-B/A secondaire. Le seul connecteur USB de la tablette est
câblé sur les pads dédiés USB_DP/DM (OTG_HS).

⇒ **Plan A (usb_host_msc sur OTG_FS) physiquement impossible sur le
JC1060P470C.** La décision se joue donc sur le **Plan B : MSC via TinyUSB
(tuh_msc) derrière un hub** branché sur le port HS (le DDJ et la clé USB
simultanés), avec la glue FatFS maison (l'exemple tuh msc_file_explorer +
CFG_TUH_HUB comme point de départ) et le budget endpoints (~9/16 sur OTG_HS).

Impact migration : `usb_storage` de main-deck-p4 ne peut PAS être migré tel
quel (usb_host_msc → pile IDF sur OTG_FS absent). Il faut soit un portage
TinyUSB-MSC (nouveau composant), soit faire la lecture clé USB via un hub
externe. À trancher au moment de la migration N2.
