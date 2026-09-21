# Ethernet IP101/RMII sur Guition JC1060P470 (ESP32-P4) — « phy is busy » : état de la recherche

Date : 2026-09-20. Recherche web ciblée + vérification dans les sources IDF/ESPHome.
Périphérie concernée : EMAC ESP32-P4 (Synopsys GMAC), PHY IP101, RMII, REF_CLK externe 50 MHz sur GPIO50.

## Résumé exécutif

1. **L'Ethernet IP101 sur les panneaux Guition P4 fonctionne réellement** — mais uniquement sur les
   **modèles « Ethernet »** (JC1060P470C / variantes avec RJ45 + caméra). Le projet `espcontrol`
   (jtenniswood) embarque une config Ethernet testée sur hardware avec exactement les pins
   MDC=GPIO31, MDIO=GPIO52, power=GPIO51, CLK_EXT_IN=GPIO50, phy_addr=1. Il existe **aussi des
   variantes sans Ethernet câblé** (JC1060P470 « jaune », Wi-Fi uniquement via C6) vendues sous un
   nom très proche — c'est la première chose à vérifier sur une board qui ne négocie rien.
2. Le symptôme « `emac_esp_2_read_phy_reg phy is busy` » à chaque lecture = **transaction MDIO qui
   ne se termine jamais dans la fenêtre de timeout de l'IDF (1000 µs seulement)**. Les causes
   documentées convergent avec l'hypothèse principale : **REF_CLK 50 MHz absente** (Espressif
   stipule « REF_CLK must be stable during any access to PHY and MAC »), ou **PHY tenue en reset /
   non alimentée** (GPIO51 = RESET_N actif bas, pas un buffer d'horloge).
3. Pas d'errata P4 spécifique au mode CLK_EXT_IN trouvé. En revanche le mode **CLK_OUT** (clock
   interne) a un conflit documenté et résolu « won't do » avec le PSRAM à 200 MHz (MPLL partagé) —
   sans rapport avec cette board qui est en EXT_IN, mais utile à connaître.
4. GPIO50 est bien une entrée IOMUX dédiée REF_CLK (avec GPIO32 et GPIO44). Pas de pull-up requis
   pour une entrée d'horloge ; pas de conflit data-plane documenté (le data-plane RMII P4 utilise
   d'autres GPIO).

---

## Q1 — L'Ethernet IP101/RMII fonctionne-t-il sur les JC1060P470 ?

### Preuves de fonctionnement (modèle Ethernet)

- **jtenniswood/espcontrol**, `common/addon/connectivity_ethernet.yaml` (vérifié dans le dépôt,
  clone du 2026-09-20) — config réellement distribuée :
  ```yaml
  ethernet:
    type: IP101
    mdc_pin: GPIO31
    mdio_pin: GPIO52
    power_pin: GPIO51
    clk:
      mode: CLK_EXT_IN
      pin: GPIO50
    phy_addr: 1
  ```
  Doc d'installation : « 7-inch JC1060P470 **Ethernet model** » et « JC1060P470 **new panel**
  Ethernet model » (V1/V2), i.e. l'Ethernet est proposé comme variante de transport à part entière.
  https://jtenniswood.github.io/espcontrol/getting-started/manual-esphome-setup
- **Issue espcontrol #508** (2026-06-15) : « this board has a built-in RJ45/IP101 Ethernet port
  (per the existing connectivity_ethernet.yaml addon, which already has correct pin definitions) ».
- **PR espcontrol #515** (2026-06-16, mergée) : « Device testing — test on a JC1060P470 with the
  built-in Ethernet port … Confirm it boots, gets an Ethernet IP address, and can be added to Home
  Assistant ». Donc une bring-up Ethernet **validée sur hardware**.
- **Issue espcontrol #647** : un utilisateur fait tourner `network_transport: ethernet` sur un
  « 7inch JC1060P470C » (capture d'écran à l'appui).
- La doc espcontrol note un effet de bord amusant mais révélateur : en mode Ethernet on monte le
  PWM du backlight à 20 kHz pour éviter un scintillement « seen during hardware testing with wired
  networking enabled » (interaction EMI/échantillonnage, preuve que l'Ethernet tourne vraiment).
- **YouTube** : « Getting Ethernet Working on ESP32-P4 (IP101 PHY) » — bring-up IP101+P4 générique
  démontrée (pas spécifique Guition).

### Variantes et confusion de noms (⇒ risque « Ethernet non câblé »)

- Le thread HA « Guition esp32-p4-jc1060p470 » (community.home-assistant.io/t/959144) montre dès le
  1er post qu'il existe **deux générations** : P4+C6 séparés (jaune, « yellow pcb ») et une version
  noire P4+C6 combinés **« This one has an Ethernet port and a camera included »** (lien AliExpress
  3256810238878747). Le 10.1 » et le modèle de base sont décrits partout comme « Wi-Fi 6 (via
  ESP32-C6) » **sans mention d'Ethernet** — cf. atomic14.com/esp32/boards/guition-jc1060p470/
  (« Connectivity: Wi-Fi 6 (via ESP32-C6), BLE 5 », champ extras : sd — pas de RJ45).
- La config ESPHome communautaire du modèle noir **JC1060P470C-I-W-Y**
  (community.home-assistant.io/t/1003085) ne contient **aucun bloc `ethernet:`** : Wi-Fi via
  esp32_hosted/C6. Cela ne prouve pas que le RJ45 soit absent, mais que **la majorité des configs
  publiées sont Wi-Fi**, et que le port Ethernet n'existe que sur certains modèles (C / « Ethernet
  model »).
- Les numéros « _I_W », « _I_W_Y » du ZIP constructeur (pan.jczn1688.com, schematics inclus) sont
  les références officielles des variantes ; le schematic ZIP est **la source à obtenir** pour
  trancher si la board physique a bien le PHY + le magnétique RJ45 montés.
- Variante matérielle supplémentaire signalée par atomic14 : « P4 silicon revision 3.0 (2026)
  changes power rails — early and late boards may differ ». ESPHome #19330 confirme l'existence de
  silicium pré-v3 nécessitant `engineering_sample: true` + `CONFIG_ESP32P4_REV_MIN_100`.

**Conclusion Q1** : oui, Ethernet fonctionne sur le modèle Ethernet (plusieurs indépendants), mais
le nom JC1060P470 couvre des variantes Wi-Fi-only. Une board sans PHY câblé/avec PHY non alimenté
produirait exactement le symptôme observé.

---

## Q2 — « phy is busy » permanent sur EMAC P4 : mécanique et causes connues

### Ce que fait exactement l'IDF (vérifié dans les sources, v6.x)

`components/esp_eth/src/mac/esp_eth_mac_esp.c` :

```c
#define PHY_OPERATION_TIMEOUT_US (1000)   // 1 ms tout compris !
...
static esp_err_t emac_esp32_read_phy_reg(...) {
    ESP_GOTO_ON_FALSE(!emac_hal_is_mii_busy(&emac->hal), ESP_ERR_INVALID_STATE, err, TAG, "phy is busy");
    emac_hal_set_phy_cmd(&emac->hal, phy_addr, phy_reg, false);
    uint32_t to = 0; bool busy = true;
    do { esp_rom_delay_us(100); busy = emac_hal_is_mii_busy(&emac->hal); to += 100; }
    while (busy && to < PHY_OPERATION_TIMEOUT_US);
    ESP_GOTO_ON_FALSE(!busy, ESP_ERR_TIMEOUT, err, TAG, "phy is busy");
    ...
}
```

Le message « phy is busy » = le bit BUSY (GMIIAR du GMAC Synopsys) ne retombe pas dans les 1 ms.
Ce n'est **pas** un « module entier figé » au sens DMA : c'est le **plan de management SMI** qui ne
complète pas. Deux lectures possibles, cumulables :
- l'EMAC ne prend jamais le coup d'horloge nécessaire au SMI (l'IDF documente : « REF_CLK must be
  stable during any access to PHY and MAC » — doc P4, section RMII) ;
- le PHY ne répond pas (non alimenté, maintenu en reset, pas de REF_CLK de son côté, mauvaise
  adresse, MDIO court-circuité/pas de pull-up).

### Causes connues et sourcées

1. **REF_CLK absente ou instable (cause n°1 ici).** Doc officielle P4 : « In RMII mode … REF_CLK
   must be stable during any access to PHY and MAC » + avertissement sur l'intégrité du signal.
   Une IP101 sans 50 MHz (ni cristal 25 MHz propre) est muette sur MDIO et le RMII ne transfère
   rien → pas d'ETHERNET_EVENT_CONNECTED, pas de DHCP. Cohérent à 100 % avec le symptôme.
2. **PHY maintenu en reset / non alimenté.** GPIO51 est le **RESET_N du PHY (actif bas)**, pas un
   buffer d'horloge (voir Q4). Si GPIO51 est laissé flottant/bloqué bas, ou si le gate d'alimentation
   du PHY dépend d'un autre GPIO jamais piloté, le PHY reste mort. IDF finit la séquence reset par
   GPIO51=HIGH, mais **asserte LOW ~100 µs à l'init** — une alim PHY qui ne remonte pas après ce
   glitch laisserait un PHY mort.
3. **Conflit MPLL / PSRAM (mode CLK_OUT uniquement).** Issue espressif/esp-idf **#18377** (mars
   2026, fermée « Won't Do ») : en `EMAC_CLK_OUT` l'EMAC doit dériver 50 MHz de la MPLL ; le PSRAM
   à 200 MHz verrouille la MPLL à 400 MHz → conflit, init EMAC échoue. La doc P4 ajoute : PSRAM à
   80 MHz ⇒ MPLL 320 MHz, aucun diviseur entier ne donne 50 MHz ±50 ppm ⇒ **EMAC init fail**. Ne
   s'applique PAS en CLK_EXT_IN (aucune MPLL nécessaire) — donc pas la cause ici, mais c'est la
   raison pour laquelle l'IDF recommande l'horloge externe quand PSRAM il y a. **Attention pour ce
   projet** : le P4 Pajoniiir tourne avec PSRAM 200 MHz ; garder CLK_EXT_IN est donc impératif.
4. **Pull-up MDIO.** Cause classique sur designs custom (thread esp32.com t=40785 « ESP32 can not
   initialize IP101 Ethernet PHY » — esp32.com protégé par bot-check, contenu non extrait ;
   l'électronique standard veut un pull-up MDIO). Si MDIO reste à 0 ou est relié à autre chose,
   le PHY ne pilote pas la trame de réponse. À vérifier au scope (le GPIO52 est aussi une broche
   RXD0 **alternative** du data-plane P4 — une erreur de mapping dataif GPU côté config peut
   l'accaparer, improbable avec la config standard).
5. **Mauvaise adresse PHY.** phy_addr=1 confirmé par espcontrol ET par le pin map Waveshare
   (identique, voir Q3/Q4) — pas suspects, mais si la board est une autre variante, un scan MDIO
   (essayer addr 0..31) tranche en 1 min.
6. **Optimisation compilateur et poll MDIO (Rust, écosystème esp-hal).** Le crate esp-p4-eth
   documente : « with opt-level = "s" inlining gets aggressive enough that the naked-counter MDIO
   BUSY-poll loop completes before the PHY answers and the bus times out ». En IDF C le timeout est
   fixe (1 ms) donc ce mode de défaillance précis ne s'applique pas, mais cela confirme que le
   poll BUSY est une zone fragile sur P4 et que le PHY a besoin d'un délai après reset avant de
   répondre (l'IDF par défaut n'a **pas** de `post_hw_reset_delay_ms`).
7. **Autres cas P4 relevés** : « wrong chip OUI » à l'install (Reddit, ESP32-P4-WIFI6-DEV-KIT —
   PHY présent mais mauvaise lecture ID, autre famille de panne MDIO) ; esphome #19330 (P4+IP101 :
   DHCP OK mais api/mqtt jamais connectés — panne *post*-lien, preuve que l'EMAC P4 sous ESPHome
   a d'autres quirks, mais cette partie du chemin n'est pas atteinte ici).

**Pas trouvé d'errata Espressif spécifique au SMI/MDIO du P4** (datasheet/errata P4 non cités dans
aucun des threads) ; le seul avertissement « errata-like » officiel est le conflit MPLL/PSRAM du
mode CLK_OUT documenté dans la doc P4 + #18377.

---

## Q3 — Mode CLK_EXT_IN sur P4 : subtilités

Sources : doc IDF P4 `api-reference/network/esp_eth.html` (stable, vérifiée 2026-09-20) + code
`esp_eth_mac_esp.c` / `esp_eth_mac_esp_gpio.c`.

- **Pins d'entrée REF_CLK autorisés en IOMUX : GPIO32, GPIO44 et GPIO50.** GPIO50 est donc bien un
  pad dédié (fonction `rmii_refclk_i`) ; le driver appelle `emac_esp_iomux_rmii_clk_input(50)` puis
  `emac_hal_clock_enable_rmii_input()` — pas de GPIO matrix, pas de PLL, rien à armer d'autre.
- **Pas de pull-up/pull-down requis** : c'est une entrée d'horloge pilotée en push-pull par
  l'oscillateur/le PHY. Aucune mention d'un pull-up interne requis dans la doc P4.
- **Aucun conflit MPLL/PSRAM** en EXT_IN (contrairement à CLK_OUT, voir Q2.3). L'IDF recommande
  même explicitement EXT_IN quand PSRAM est utilisé.
- **Conflits de périphs** : GPIO50 n'apparaît pas dans le data-plane RMII P4 (table : TX_EN
  33/40/49, TXD0 34/41, TXD1 35/42, CRS_DV 28/45/**51**, RXD0 29/46/**52**, RXD1 30/47/**53**).
  NB : GPIO51/52 sont des alternatives data-plane, mais MDC/MDIO (control plane) se routent via
  GPIO matrix sur n'importe quel GPIO libre — pas de conflit tant qu'on n'active pas un dataif
  alternatif. Sur la Guition, GPIO50/51/52 ne sont revendiqués par rien d'autre (les configs
  espcontrol utilisent GPIO18/19/14–17 pour le SDIO du C6, GPIO23 backlight, GPIO7/8 I2C, GPIO54
  reset C6).
- Le pin map P4 **identique** (50=REF_CLK in, 51=PHY reset actif bas, 52=MDIO, 31=MDC,
  34/35=TXD0/1, 49=TX_EN, 30/29=RXD1/0, 28=CRS_DV, addr 1) est documenté comme config de référence
  par le crate esp-p4-eth (Waveshare ESP32-P4-ETH, IP101GRI) — Guition a manifestement repris le
  même design RMII. Les deux board-sets partagent donc les mêmes contraintes.
- **Signal integrity** : la doc insiste (« keep the trace as short… »). Sur un panneau commercial
  ça se joue sans intervention ; en cas de doute, vérifier la stabilité de l'horloge à l'oscillo
  pendant et après le reset du PHY.

---

## Q4 — GPIO51 : reset du PHY, pas un buffer d'horloge ; gestion exacte du power_pin ESPHome

- **Rôle réel de GPIO51** : d'après le pin map esp-p4-eth/Waveshare (même design RMII),
  « PHY RESET | 51 (active low) ». C'est un **reset actif bas**, pas un enable d'oscillateur ni un
  buffer d'horloge. (Si la board était équipée d'un oscillateur dédié, son enable serait un autre
  GPIO ; aucun indice trouvé en ce sens. Le schematic du ZIP constructeur reste l'arbitre.)
- **Chaîne ESPHome** (vérifiée dans `esphome/components/ethernet/`, branche dev) :
  - `__init__.py` : `power_pin` est lu tel quel et passé au composant ;
  - `ethernet_component_esp32.cpp` : `phy_config.reset_gpio_num = this->power_pin_;` — le power_pin
    **devient le GPIO de reset du PHY IDF**. Aucune inversion supportée (`inverted` du pin schema
    n'est pas propagé) : la polarité est celle de l'IDF (actif bas) ;
  - `setup()` : `delay(300)` « to allow power to stabilise » avant `esp_eth_driver_install` —
    ESPHome ne fait **aucune** séquence de puissance propre au power_pin lui-même.
- **Chaîne IDF** (`esp_eth_phy_802_3.c`, driver 802.3 générique utilisé pour IP101) :
  `esp_eth_phy_802_3_reset_hw()` : GPIO en sortie, **niveau 0 pendant `hw_reset_assert_time_us`
  (défaut 100 µs), puis niveau 1** ; `post_hw_reset_delay_ms` par défaut = 0 (aucune attente
  après la remontée). La séquence se termine donc bien par GPIO51=HIGH (PHY hors reset), ce qui
  correspond à l'observation « piloté HIGH ».
- **Implication diagnostique** : à l'init, GPIO51 est brièvement tiré **bas** (100 µs). Si un
  instrument mesure un GPIO51 bloqué bas ou flottant avant/pendant l'init, ou si le PHY n'a pas
  fini de sortir de reset quand la première lecture MDIO démarre (pas de post-reset delay par
  défaut), on obtient des « phy is busy ». Un contournement simple si suspecté : `power_pin` avec
  `post_hw_reset_delay_ms` via `eth_phy_config_t` en IDF pur, ou en ESPHome un `output:` GPIO51
  piloté HIGH au boot avant l'init Ethernet… (en ESPHome pur, il n'y a pas d'option
  post-reset-delay exposée — à noter).

---

## Options classées (prochaines actions)

1. **Identifier la variante exacte de la board** (gratuit, bloquant) : présence physique du RJ45 +
  silkscreen (`JC1060P470C` vs `JC1060P470`), marquage V2/date-code (≥2622 = V2). Si la board est
  la variante Wi-Fi-only, le PHY n'existe pas et le symptôme est normal. Récupérer le schematic
  dans le ZIP constructeur (pan.jczn1688.com, `JC1060P470C_I_W.zip` / `_I_W_Y.zip`).
2. **Mesurer REF_CLK sur GPIO50** (scope/logic analyzer) : 50 MHz stable **pendant** l'init et
  après. Si absent → chercher l'oscillateur/le gate d'alim côté board, ou PHY jamais sorti de
  reset. C'est le test qui confirme/infirme l'hypothèse principale.
3. **Vérifier GPIO51 au boot** : normalement HIGH en permanence sauf 100 µs à l'init IDF. S'il est
  bas/flottant, le PHY reste en reset → « phy is busy » garanti.
4. **Scope sur MDC (GPIO31) et MDIO (GPIO52)** pendant une lecture : si MDC ne toggle pas →
  problème de clocking EMAC côté P4 ; si MDC toggle mais MDIO reste haut sans trame → PHY muet
  (adresse, pull-up, alim). Tester aussi un scan d'adresse 0–31.
5. **Rapprocher la config de la config espcontrol qui marche** (déjà identique : type IP101,
  31/52/51, EXT_IN sur 50, addr 1) et vérifier le sdkconfig : PSRAM en EXT_IN n'a pas d'impact,
  mais ne pas passer en CLK_OUT (conflit MPLL/PSRAM 200 MHz documenté).
6. Si tout est bon côté hardware mais que MDIO reste muet : tenter un `post_hw_reset_delay_ms`
  (le PHY IP101 peut avoir besoin de ms après reset avant de répondre) et vérifier la pull-up MDIO
  (~10 kΩ vers 3.3 V selon le design ; le schematic tranchera).
7. Garder en réserve : `phy_registers` ESPHome / `esp_eth_phy_802_3` pour forcer des registres
  IP101 spécifiques une fois la lecture MDIO fonctionnelle.

## Limites de la recherche

- esp32.com (t=46319 « ESP IDF and JC1060P470 », t=40785 « ESP32 can not initialize IP101 ») :
  protégé par un bot-check, contenu non extrait (navigateur indisponible dans cette session).
- Les pages 2–3 du thread HA 959144 n'ont pas livré leur texte intégral via extraction ; les
  preuves Ethernet reposent surtout sur espcontrol (YAML + issues + doc) et le pin map esp-p4-eth.
- Aucun errata Espressif P4 spécifique SMI/EMAC trouvé publiquement ; à confirmer dans le PDF
  errata officiel si besoin.

## Sources principales

- IDF P4 Ethernet API (stable) : https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/network/esp_eth.html
- IDF `esp_eth_mac_esp.c` (timeout 1 ms, « phy is busy ») et `esp_eth_phy_802_3.c` (reset_hw) : https://github.com/espressif/esp-idf
- espressif/esp-idf issue #18377 (RMII CLK_OUT + PSRAM/MPLL, Won't Do) : https://github.com/espressif/esp-idf/issues/18377
- espcontrol — config Ethernet testée : https://jtenniswood.github.io/espcontrol/getting-started/manual-esphome-setup et `common/addon/connectivity_ethernet.yaml`
- espcontrol issues #508, #515 (validation hardware Ethernet JC1060P470), #647 (JC1060P470C Ethernet en prod utilisateur)
- esp-p4-eth (pin map Waveshare/Guition identique, contraintes MDIO/busy, DMA < 0x4FF80000) : https://github.com/babasha/esp-p4-eth / docs.rs/esp-p4-eth
- ESPHome Ethernet component (power_pin → reset_gpio_num) : https://esphome.io/components/ethernet/ + `esphome/components/ethernet/ethernet_component_esp32.cpp`
- HA community 959144 (variantes JC1060P470, Ethernet sur le modèle combiné) et 1003085 (JC1060P470C-I-W-Y, config Wi-Fi sans Ethernet)
- atomic14 (fiche board, variantes, silicium rev 3.0) : https://www.atomic14.com/esp32/boards/guition-jc1060p470/
- ESPHome issue #19330 (P4+IP101 post-link quirk, silicium pré-v3) : https://github.com/esphome/esphome/issues/19330

---

## Bilan debug nocturne 2026-09-20 (v97-v104, Pajoniiir)

1. **Chaîne vprintf réparée** (v97/v98) : console_vprintf capture son prev et
   log_screen_rehook re-capture toujours - la chaîne ls -> console -> UART
   est linéaire. ⚠️ Ne PAS re-capturer console dans ls ET ls dans console
   (récursion infinie -> stack protection fault, observé).
2. **MDIO scan fonctionnel** (v100) : `mac->read_phy_reg(mac, ...)` - le
   premier argument est l'objet MAC, pas le handle eth (containerof).
   Résultat : **addr=1 répond, ID=0x02430c54 = IP101GR** - le bus MDIO et le
   PHY sont vivants.
3. **Espoirs écartés par A/B sur hardware** :
   - USB OFF (v96) : pas de changement
   - Display/DSI/LVGL OFF (v101, v103 fix assert lvgl_port_unlock) : rien
   - Audio ES8311/I2S OFF (v102) : rien
   - FREERTOS_HZ 100 (v104) : rien (esp_hosted recommande même 1000)
   - PSRAM 200M HEX+XIP dans l'exemple vendor : l'ETH marche quand même
4. **État** : Pajoniiir v104 = app minimale (ETH+log), step 6 atteint, PHY
   vivant, mais aucun event ETHERNET_EVENT_START/CONNECTED ni DHCP. Le même
   exemple IDF 6.0.2 obtient Link Up + IP en 3-4 s.
5. **Restant** : (a) diff bit-à-bit sdkconfig exemple vs Pajoniiir (le diff
   initial ne montre rien d'évident côté ETH), (b) tester les ISR cache-safe
   ETH (CONFIG_ETH_ISR_CACHE_SAFE ?), (c) porter l'exemple vers le composant
   `espressif/ip101` du registry, (d) suspect restant : une tâche/timer de
   l'app qui affame le poll de link du generic PHY (cf. #184 mono-core).

## Complément nuit (v105, logs DEBUG eth)

Même avec DEBUG sur eth_phy_802_3/emac_esp/netif_glue : **aucun log du stack
ETH après le start** - pas d'autonego, pas de timeout autonego, pas d'event
ETHERNET_EVENT_START. Le PHY répond au MDIO (scan), donc le plan de
management fonctionne, mais le driver ne lance jamais son cycle de link.

⇒ Le suspect n°1 devient un composant LIÉ au build (pas démarré) qui
interfère : esp_hosted (lié, non démarré), tinyusb (lié, non démarré en
v101+), lvgl, ou un hook global. Test du matin : construire l'exemple vendor
en ajoutant les composants liés de Pajoniiir un par un (esp_hosted, tinyusb,
lvgl) dans un projet hybride qui COMPILE (le test hybride audio a échoué sur
un include, à reprendre avec les bons includes: board.h/audio.h).
Alternative rapide : désactiver les composants dans main-deck-jc1060
(idf_component.yml / CMakeLists) un par un et mesurer.

## Fin de session nocturne — bilan et plan de reprise

Tests réalisés cette nuit (tous flashés + vérifiés sur hardware) :
| Version | Config | Résultat ETH |
|---|---|---|
| v97/v98 | USB OFF + chaîne vprintf réparée | crash (récursion tees, corrigé) puis Load fault |
| v99/v100 | stack main 16 KB + scan MDIO (bon handle) | **MDIO scan OK : IP101GR @ addr 1, ID 0x02430c54** |
| v101/v103 | + display OFF, + audio OFF | scan OK, toujours pas de link event |
| v104 | + FREERTOS_HZ=100 | idem |
| v105 | + DEBUG logs eth | idem — aucun log du stack eth après start |

Constats fermes :
- PHY IP101GR vivant, bus MDIO OK, adresse 1 confirmée (ID 0x02430c54).
- IDF 6.0.2 est capable de faire l'ETH sur cette board (exemple vendor :
  Link Up + IP 192.168.100.132, y compris avec PSRAM 200M HEX+XIP).
- Donc le blocage est spécifique au build Pajoniiir : différence dans le
  sdkconfig global ou un composant LIÉ (non démarré) qui interfère.

Plan de reprise (dans l'ordre) :
1. A/B sdkconfig : compiler l'exemple vendor avec le sdkconfig.defaults
   complet de Pajoniiir (PKT_STATS, cache L2, C2M chunked, etc.). Le diff
   sdkconfig initial n'a rien montré côté ETH — approfondir (ISR, prio).
2. Retirer de Pajoniiir les composants LIÉS un par un : esp_hosted,
   tinyusb (usb_tu_app lié), lvgl — mesurer à chaque retrait.
3. Si tout échoue : comparer le comportement GPIO50 (REF_CLK) à l'oscillo.
4. Considérer la voie espcontrol : compiler le variant ethernet ESPHome
   (network_transport: ethernet) comme référence croisée matérielle.

Ne pas oublier : le test croisé vendor IDP 5.5.4 = Link Up + IP CONFIRMÉ
(capture dans l'historique session) — le hardware est sain.

## Test A/B n°1 (sdkconfig complet de Pajoniiir dans l'exemple vendor)

Résultat : l'exemple ne boot PAS avec le sdkconfig complet de Pajoniiir
(figé à ~1,2 s, avant app_main — mais test non concluant tel quel : la
partition table custom + tous les composants absents de l'exemple rendent
la comparaison invalide).

Conclusion de la nuit : l'approche "config entière d'un coup" ne permet pas
d'isoler. Reprendre par bissection de groupes d'options (PSRAM, PKT_STATS,
cache, FREERTOS) DANS l'exemple vendor, ou par ajout progressif des
composants Pajoniiir dans l'exemple (audio init d'abord — include BSP:
board.h/audio.h, EXTRA_COMPONENT_DIRS /pajoniiir/... nécessite fullclean).

## État hardware laissé en place
- Tablette : dernier flash = exemple vendor avec sdkconfig Pajoniiir (boot
  figé) → AU MATIN : reflasher Pajoniiir v104 (build/ prêt) avant tout test.
- Exemple vendor fonctionnel validé : /tmp/jcy (IDF 5.5.4) et /tmp/eth_ab
  (IDF 6.0.2, défauts propres) = Link Up + Got IP confirmés.
