# JC1060P470C_I_W_Y - Bring-Up Guide

## Phase 1 : Bring-up Matériel (complète)

Cette phase couvre l'initialisation du matériel de base : écran, tactile, audio.

---

## 📁 Structure créée

```
firmware/main-deck-jc1060/
├── components/
│   └── bsp_jc1060p470/
│       ├── include/bsp/
│       │   ├── bsp_board_config.h    # Configuration GPIO, timings LCD
│       │   ├── bsp_common.h          # API commune
│       │   ├── display.h             # API affichage
│       │   ├── touch.h               # API tactile
│       │   └── audio.h               # API audio
│       ├── src/
│       │   ├── bsp_board.c           # Initialisation carte
│       │   ├── bsp_display.c         # Pilote JD9165
│       │   ├── bsp_touch.c           # Pilote GT911
│       │   └── bsp_audio.c           # Pilote ES8311
│       ├── CMakeLists.txt
│       └── idf_component.yml
├── examples/
│   └── esp_draw_bit/
│       ├── main/
│       │   ├── main.c                # Application de test
│       │   └── CMakeLists.txt
│       ├── CMakeLists.txt
│       ├── README.md
│       └── sdkconfig.defaults
└── sdkconfig.defaults
```

---

## 🔧 Configuration matérielle

### Écran JD9165
| Paramètre | Valeur |
|-----------|--------|
| Résolution | 1024×600 (paysage natif) |
| Pixel clock | 51.2 MHz |
| MIPI-DSI lanes | 2 @ 1.0 Gbps |
| GPIO Reset | GPIO5 |
| GPIO Backlight | GPIO23 (PWM) |

### Touch GT911
| Paramètre | Valeur |
|-----------|--------|
| Adresse I2C | 0x5D |
| I2C SDA | GPIO14 (software) |
| I2C SCL | GPIO15 (software) |
| GPIO INT | GPIO7 |
| GPIO RST | GPIO8 |

### Audio ES8311
| Paramètre | Valeur |
|-----------|--------|
| Adresse I2C | 0x18 |
| I2S SCLK | GPIO13 |
| I2S LRCK | GPIO12 |
| I2S DOUT | GPIO9 |
| PA Enable | GPIO1 |

---

## ⚠️ Conflit GPIO12 résolu

**Problème** : GPIO12 utilisé pour I2S LRCK **et** I2C SDA

**Solution implémentée** : I2C logiciel (bit-banged) sur GPIO14/15

```c
// Dans bsp_board_config.h
#define BSP_USE_SW_I2C_FOR_TOUCH  (1)
#define BSP_USE_SW_I2C_FOR_AUDIO  (1)
#define BSP_I2C_SW_SDA_GPIO       (GPIO_NUM_14)
#define BSP_I2C_SW_SCL_GPIO       (GPIO_NUM_15)
```

---

## 🚀 Instructions de build

### 1. Préparer l'environnement ESP-IDF

```bash
# ESP-IDF v6.0.2 requis
. $HOME/esp/esp-idf-v6.0.2/export.sh
idf.py --version  # Doit afficher "ESP-IDF v6.0.2"
```

### 2. Builder l'exemple esp_draw_bit

```bash
cd firmware/main-deck-jc1060/examples/esp_draw_bit

# Configurer la cible
idf.py set-target esp32p4

# Compiler
idf.py build
```

**Sortie attendue** :
```
Project 'esp_draw_bit' successfully built
Build complete in 45s
```

### 3. Flasher sur la carte

**Linux** :
```bash
idf.py -p /dev/ttyACM0 flash monitor
```

**Windows** :
```powershell
idf.py -p COM15 flash monitor
```

### 4. Flash / monitor via Docker (carte unique, sans IDF natif installé)

Cette variante est utilisée lorsque l'hôte n'a pas d'ESP-IDF natif et que la
carte est la seule pouvant exposer la console. Tout se passe dans le conteneur
`espressif/idf:v6.0.2`.

Flash :

```bash
cd firmware/main-deck-jc1060/examples/esp_draw_bit
docker run --rm --user root --group-add dialout -e HOME=/tmp \
  --device=/dev/ttyACM0 \
  -v "$(git rev-parse --show-toplevel):/host" \
  -w /host/firmware/main-deck-jc1060/examples/esp_draw_bit \
  espressif/idf:v6.0.2 bash -lc \
  'source /opt/esp/idf/export.sh >/dev/null && idf.py -p /dev/ttyACM0 flash'
```

Monitoring (rebondit le log de boot en direct) :

```bash
docker run --rm --user root --group-add dialout -e HOME=/tmp \
  --device=/dev/ttyACM0 \
  -v "$(git rev-parse --show-toplevel):/host" \
  -w /host/firmware/main-deck-jc1060/examples/esp_draw_bit \
  espressif/idf:v6.0.2 bash -lc \
  'source /opt/esp/idf/export.sh >/dev/null && idf.py -p /dev/ttyACM0 monitor'
```

Pour sortir du monitor, taper `ctrl+]`. En scriptshell non-interactif, encadrer
avec `timeout N` pour borner la capture.

> Le conteneur est lancé en `root` uniquement pour accéder au device USB
> (`/dev/ttyACM0` appartient à `root:dialout`). Utiliser
> `--group-add dialout` seul ne suffit pas toujours selon la version de docker ;
> `--device=/dev/ttyACM0` expose le nœud hôte tel quel.

---

## ✅ Checklist de validation

### Boot log attendu

```
I (298) cpu_start: Starting app_cpu0
I (307) cpu_start: Starting app_cpu1
I (307) cpu_start: Calling app_main()

I (314) draw_bit: JC1060P470C Draw Bitmap Test
I (319) draw_bit: ============================
I (324) bsp_board: Initializing board JC1060P470C_I_W_Y
I (331) bsp_board: Software I2C initialized on GPIO14 (SDA), GPIO15 (SCL)
I (342) bsp_board: Board initialization complete
I (348) bsp_board: Board: JC1060P470C_I_W_Y

I (354) bsp_display: Starting display with config: 1024x600, buffer=51200
I (363) bsp_display: MIPI-DSI bus created: 2 lanes @ 1000 Mbps
I (371) bsp_display: JD9165 panel created
I (492) bsp_display: Display initialized: 1024x600 @ 60Hz
I (498) bsp_display: LVGL display initialized
I (504) draw_bit: Display initialized

I (510) bsp_touch: Initializing I2C for touch on GPIO14/15
I (518) bsp_touch: GT911 touch initialized: 1024x600, swap=0, mirror_x=0, mirror_y=0
I (530) draw_bit: Touch initialized

I (536) bsp_audio: Initializing audio
I (542) bsp_audio: I2S initialized: 44100 Hz, 16-bit, Stereo
I (550) bsp_audio: ES8311 codec initialized
I (556) bsp_audio: Volume set to -20 dB
I (562) bsp_audio: Audio initialization complete
I (569) draw_bit: Audio initialized

I (574) bsp_board: Backlight on
I (580) draw_bit: Backlight on
I (585) draw_bit: Test pattern 0
```

### Tests visuels

1. **Barres de couleurs** (8 bandes verticales)
   - Noir, Bleu, Vert, Cyan, Rouge, Magenta, Jaune, Blanc
   
2. **Gradient horizontal** (gauche → droite)

3. **Gradient vertical** (haut → bas)

4. **Écrans solides** (Rouge, Vert, Bleu alternés)

### Test tactile

Toucher l'écran → cercles blancs doivent apparaître + logs série :
```
I (xxx) draw_bit: Touch: (512, 300) strength=128
I (yyy) draw_bit: Touch: (720, 150) strength=135
```

---

## 🔍 Diagnostic des problèmes

### Écran noir

**Cause 1** : Backlight non alimenté
```bash
# Mesurer tension sur GPIO23
multimeter --voltage GPIO23-GND  # Doit être ~3.3V
```

**Cause 2** : Séquence d'initialisation JD9165 incorrecte
- Vérifier `jd9165_init_cmds[]` dans `bsp_display.c`
- Comparer avec datasheet JC1060P470C

**Cause 3** : MIPI-DSI lanes mal configurées
- Vérifier `BSP_LCD_MIPI_DSI_LANE_NUM` (doit être 2)
- Vérifier `BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS` (1000)

### Tactile ne répond pas

**Cause 1** : I2C ne fonctionne pas
```bash
# Scanner bus I2C
idf.py -p /dev/ttyACM0 monitor  # Chercher "scanning I2C"
# Doit trouver 0x5D (GT911) et 0x18 (ES8311)
```

**Cause 2** : GPIO14/15 mal configurés
- Vérifier `BSP_I2C_SW_SDA_GPIO` et `BSP_I2C_SW_SCL_GPIO`
- Mesurer continuité avec multimètre

**Cause 3** : GT911 reset pin
- Vérifier GPIO8 (reset) → doit pulser au démarrage

### Audio avec bruit

**Cause 1** : Conflit I2S/I2C
- Vérifier que I2C logiciel est activé
- Mesurer signaux I2S sur GPIO13/12/9 avec oscilloscope

**Cause 2** : Sample rate incorrect
- Tester avec 48000 Hz au lieu de 44100 Hz
- Vérifier `CONFIG_I2S_STD_CLK_DEFAULT_CONFIG`

**Cause 3** : PA enable non activé
- Mesurer tension sur GPIO1 (doit être HIGH)

---

## 📊 Prochaines étapes après validation

### Phase 2 : Portage UI LVGL (2-3 semaines)

1. **Copier composants UI de Pajoniiir** :
   ```
   firmware/main-deck-p4/components/ui/
     → firmware/main-deck-jc1060/components/ui/
   ```

2. **Adapter pour 1024×600** :
   - Redimensionner tous les layouts
   - Ajuster waveforms (1024px largeur)
   - Modifier `ui_theme.h`

3. **Supprimer rotation PPA** :
   - Natif paysage → pas de rotation nécessaire
   - Simplifier `ui_lvgl_backend.c`

### Phase 3 : Intégration DDJ-400 (1-2 semaines)

1. **Copier parser MIDI DDJ-400** :
   ```
   controllers/pioneer_ddj_400/
     → firmware/main-deck-jc1060/controllers/
   ```

2. **Adapter UART control-link** :
   - Vérifier pins UART1 (GPIO28/29)
   - Tester avec ESP32-S3

3. **Tests fonctionnels** :
   - Hot cues (6 vs 8)
   - Loop manuel
   - Beat FX

### Phase 4 : Validation finale (1-2 semaines)

1. **Soak test audio** (5+ heures)
2. **Test thermique** enclosure
3. **Validation USB library**
4. **Tag RC1-jc1060p470c**

---

## 📝 Notes de bench

### Matériel requis
- ✅ Carte JC1060P470C_I_W_Y
- ✅ Câble USB-C (données + power)
- ✅ PC avec ESP-IDF v6.0.2
- ✅ Multimètre (debug GPIO)
- ✅ Oscilloscope (optionnel, debug I2S/MIPI)

### Fichiers de log
Sauvegarder les logs de boot :
```bash
idf.py -p /dev/ttyACM0 monitor > boot_log_$(date +%Y%m%d_%H%M%S).txt
```

### Photos/vidéos
Documenter :
- Écran allumé (chaque test pattern)
- Réaction tactile
- Logs série

---

## 🆘 Support

En cas de problème bloquant :
1. Vérifier logs série (niveau DEBUG)
2. Mesurer tensions GPIO critiques
3. Comparer avec démo Guition `lvgl_demo_v9`
4. Consulter `docs/bring_up_issues.md` (à créer)

---

**Statut Phase 1** : ✅ **Bring-up matériel validé** (2026-09-19, bench physique)

- Écran JD9165 : OK — MIPI-DSI 2 lanes @ 1000 Mbps, 1024x600 @ 59 Hz, patterns en boucle sans WDT.
  Root cause du WDT initial : rail analogique du PHY DSI = LDO interne VO3 (canal 3, 2.5 V) non acquis.
- Touch GT911 : OK — bus I2C partagé SDA=GPIO7/SCL=GPIO8, 821 events sur toute la surface (0-1023 / 0-599).
- Audio ES8311 : OK — bip test 440 Hz audible via NS4150 (PA-CTRL=GPIO11). Adresse esp_codec_dev en forme 8 bits (0x30).
- I2S : BCLK=12, WS=10, MCLK=13 (requis par l'ES8311), DOUT=9, DIN=48.
- Pattern 3 : OK (fill ligne par ligne, plus d'allocation DMA 1,2 Mo).
- Pin map de référence : repo `p1ngb4ck/unofficial_guition_esp32p4_repo`, `JC1060P470_I_W_Y/esphome_yaml_example/`.
- ⚠️ Pas encore validés : SDMMC, ESP32-C6/ESP-Hosted (SDIO 14-19), UART control link, USB host (pins DM/DP du BSP à revoir — 19 est la CMD SDIO du C6).
