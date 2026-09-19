# Phase 1 — Bring-Up Plan: JC1060P470C_I_W_Y

Guide pas-à-pas pour la première mise sous tension et validation matérielle de
la carte Guition JC1060P470C_I_W_Y. Conçu pour un nouvel arrivant sur le projet.

## Matériel requis

- Carte Guition JC1060P470C_I_W_Y (ESP32-P4 + ESP32-C6 copro WiFi)
- Câble USB-C (données + puissance)
- PC avec ESP-IDF v6.0.2 installé
- Multimètre (pour debug GPIO si besoin)
- Optionnel : oscilloscope pour debug I2S/MIPI

## Aperçu des composants

La carte intègre quatre sous-systèmes à valider un par un :

| Sous-système | Puce | Rôle |
| --- | --- | --- |
| Affichage | JD9165 | Panel MIPI-DSI 1024×600 paysage |
| Tactile | GT911 | Capteur capacitif I2C |
| Audio | ES8311 | Codec I2S (DAC + ampli NS4150) |
| Co-processeur | ESP32-C6 | WiFi via ESP-Hosted SDIO |

La carte comporte également un port USB host pour la bibliothèque musique, un
slot microSD et une liaison UART vers l'ESP32-S3 (control-link).

## Branchements

### Alimentation et programmation

Brancher un seul câble USB-C entre le PC et le port USB-C de la carte. Ce port
fait à la fois l'alimentation et la programmation (USB-Serial-JTAG natif du P4).
Sur Windows, la carte apparaît comme un `COMx` dans le Gestionnaire de
périphériques.

### Pinout interne (déjà câblé sur la carte, pas de soudures nécessaires)

Tous les GPIO ci-dessous proviennent de `bsp_board_config.h`.

#### Affichage JD9165 (MIPI-DSI)

- Reset : `GPIO5`
- Backlight PWM : `GPIO23`
- 2 lanes MIPI-DSI @ 1.0 Gbps
- Pixel clock : 51.2 MHz
- Résolution : 1024×600, RGB565

#### Tactile GT911

- I2C logiciel (bit-bang) : SDA=`GPIO14`, SCL=`GPIO15`
- Adresse I2C : `0x5D`
- Interruption : `GPIO7`
- Reset : `GPIO8`

#### Audio ES8311 + ampli NS4150

- I2C logiciel (partagé avec tactile) : SDA=`GPIO14`, SCL=`GPIO15`
- Adresse I2C codec : `0x18`
- I2S SCLK : `GPIO13`
- I2S LRCK : `GPIO12`
- I2S DOUT : `GPIO9`
- Ampli PA Enable : `GPIO1`
- Format : 44100 Hz, 16-bit, stéréo

> ⚠️ **Conflit GPIO12** : I2S LRCK partageait la pin avec I2C SDA. Résolu par
> I2C logiciel sur GPIO14/15. Déjà codé dans la BSP — rien à faire de ton côté.

#### USB Host (bibliothèque musique)

- D- : `GPIO19`, D+ : `GPIO20`

#### MicroSD

- D0-D3 : `GPIO39-42`, CMD : `GPIO44`, CLK : `GPIO43`

#### UART control-link (vers S3)

- TX : `GPIO28`, RX : `GPIO29`, baudrate : 460800

#### Co-processeur WiFi C6

- Reset C6 : `GPIO54`
- Communication : SDIO (pins à vérifier depuis le schéma)

## Installation logicielle

1. Initialiser ESP-IDF v6.0.2 :

```powershell
. C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
idf.py --version
```

Doit afficher `ESP-IDF v6.0.2`.

2. Aller dans le dossier de l'exemple :

```powershell
$repoRoot = git rev-parse --show-toplevel
Set-Location "$repoRoot\firmware\main-deck-jc1060\examples\esp_draw_bit"
```

## Build et flash

3. Configurer la cible :

```powershell
idf.py set-target esp32p4
```

4. Compiler :

```powershell
idf.py build
```

Sortie attendue : `Project 'esp_draw_bit' successfully built`.

5. Flasher et monitorer (remplacer `COMx` par ton port) :

```powershell
idf.py -p COMx flash monitor
```

## Validation étape par étape

Chaque étape correspond à une section du boot log. Si une ligne manque ou
affiche une erreur, passer au dépannage.

### Étape A — Boot et init carte

Vérifier dans le log série :

```
bsp_board: Initializing board JC1060P470C_I_W_Y
bsp_board: Software I2C initialized on GPIO14 (SDA), GPIO15 (SCL)
bsp_board: Board initialization complete
```

Carte détectée, I2C logiciel opérationnel.

### Étape B — Affichage

Vérifier :

```
bsp_display: Starting display with config: 1024x600, buffer=51200
bsp_display: MIPI-DSI bus created: 2 lanes @ 1000 Mbps
bsp_display: JD9165 panel created
bsp_display: Display initialized: 1024x600 @ 60Hz
bsp_display: LVGL display initialized
bsp_board: Backlight on
```

Backlight allumé, panel initialisé.

Observer ensuite les patterns visuels à l'écran :

1. 8 barres de couleur verticales (noir, bleu, vert, cyan, rouge, magenta,
   jaune, blanc)
2. Gradient horizontal (gauche → droite)
3. Gradient vertical (haut → bas)
4. Écrans solides alternés (rouge, vert, bleu)

Tous les patterns doivent s'afficher correctement.

### Étape C — Tactile

Vérifier :

```
bsp_touch: Initializing I2C for touch on GPIO14/15
bsp_touch: GT911 touch initialized: 1024x600, swap=0, mirror_x=0, mirror_y=0
```

Driver GT911 chargé.

Toucher l'écran : des cercles blancs doivent apparaître à l'endroit touché, et
le log doit afficher :

```
draw_bit: Touch: (512, 300) strength=128
```

Les coordonnées doivent correspondre à la position du doigt.

### Étape D — Audio

Vérifier :

```
bsp_audio: Initializing audio
bsp_audio: I2S initialized: 44100 Hz, 16-bit, Stereo
bsp_audio: ES8311 codec initialized
bsp_audio: Volume set to -20 dB
bsp_audio: Audio initialization complete
```

Codec initialisé. Mesurer la tension sur `GPIO1` (PA Enable) — doit être HIGH
(~3.3 V).

### Étape E — Stabilité

Laisser tourner 2-3 minutes. Vérifier :

- Pas de `DSI underrun` dans le log
- Pas de `watchdog` ou `panic`
- Pas de reset spontané

Carte stable.

## Dépannage rapide

- **Écran noir** : mesurer la tension sur `GPIO23` (backlight) — doit être
  ~3.3 V. Vérifier que `BSP_LCD_MIPI_DSI_LANE_NUM=2`.
- **Tactile ne répond pas** : scanner le bus I2C — doit trouver `0x5D` (GT911)
  et `0x18` (ES8311). Vérifier que `GPIO8` (reset GT911) pulse au démarrage.
- **Audio bruité** : confirmer que l'I2C logiciel est activé
  (`BSP_USE_SW_I2C_FOR_AUDIO=1`). Tester 48000 Hz au lieu de 44100 Hz.
- **Build échoue** : confirmer `idf.py --version` donne bien v6.0.2. Effacer
  `build/` et `managed_components/` puis recommencer `set-target`.

## Statut logiciel (build)

La migration ESP-IDF 6.0.2 / LVGL 9 de l'exemple `esp_draw_bit` est validée :
build propre sous `espressif/idf:v6.0.2` (Linux, via Docker) sans erreur ni
warning. Le binaire généré est
`firmware/main-deck-jc1060/examples/esp_draw_bit/build/esp_draw_bit.bin`
(app ~0xa37 Ko, ~36 % libre). Composants managés épinglés dans
`dependencies.lock` (LVGL 9.6, esp_lcd_jd9165 2.0.2, esp_codec_dev 1.6.2,
esp_lcd_touch 1.2.1).

Attention : `sdkconfig.defaults` de l'exemple force le target ESP32-P4, le
PSRAM octal 16MB (HEX) requis par le framebuffer DPI, le support ES8311 et le
silicon « less than v3 » de la carte de production. Ne pas réactiver les
options LVGL8 (`CONFIG_LVGL_VERSION_8`) ni l'ancienne config DPI
(`pixel_clock_hz`/`timing`) : elles sont obsolètes.

Restent à réaliser physiquement : le flash et la checklist des critères de
sortie ci-dessous (patterns, tactile, audio, stabilité).

## Critères de sortie Phase 1

- [ ] Boot log correspond à la séquence attendue (display, touch, audio,
      backlight)
- [ ] Les 4 patterns visuels rendent correctement
- [ ] Les coordonnées tactiles correspondent à la position du doigt
- [ ] Le codec audio s'initialise sans bruit
- [ ] Aucun DSI underrun, watchdog ou reset pendant 2-3 minutes
