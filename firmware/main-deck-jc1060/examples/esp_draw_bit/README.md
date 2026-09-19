# ESP Draw Bit Test - JC1060P470C_I_W_Y

Test application for validating hardware bring-up of the Guition JC1060P470C_I_W_Y display board.

## Features Tested

- ✅ Display initialization (JD9165 MIPI-DSI)
- ✅ Touch screen (GT911 I2C)
- ✅ Backlight control (PWM)
- ✅ Audio codec (ES8311) - optional
- ✅ Graphics rendering (solid colors, gradients, color bars)
- ✅ Touch input monitoring

## Building

Prérequis : **ESP-IDF v6.0.2** est obligatoire (les composants managés pinent
`idf: >=6.0` et le code utilise l'API `esp_lcd_mipi_dsi.h` d'IDF 6.x).

Sous Linux/macOS (recommandé, via le conteneur officiel) :

```bash
cd firmware/main-deck-jc1060/examples/esp_draw_bit
rm -rf build sdkconfig sdkconfig.old managed_components   # une seule fois
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$(git rev-parse --show-toplevel):/host" -w /host/firmware/main-deck-jc1060/examples/esp_draw_bit \
  espressif/idf:v6.0.2 bash -lc 'source /opt/esp/idf/export.sh >/dev/null \
    && idf.py set-target esp32p4 && idf.py build'
```

Sous Windows, avec le profil ESP-IDF 6.0.2 :

```powershell
. C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
$repoRoot = git rev-parse --show-toplevel
Set-Location "$repoRoot\firmware\main-deck-jc1060\examples\esp_draw_bit"
idf.py set-target esp32p4
idf.py build
```

Notes :

- `sdkconfig.defaults` configure le target ESP32-P4, le PSRAM octal 16MB (HEX)
  nécessaire au framebuffer DPI (1024×600 RGB565 ≈ 1,2 Mo) et le support
  codec ES8311.
- LVGL 9 est fourni via le composant managé `lvgl/lvgl ^9` (le `sdkconfig`
  résultat le rapporte en `CONFIG_LVGL_VERSION_MAJOR=9`). Ne pas réactiver les
  options LVGL8 (`CONFIG_LVGL_VERSION_8`) : elles sont obsolètes.

## Flashing

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Or on Windows:
```powershell
idf.py -p COM15 flash monitor
```

No native IDF on the host? Everything happens in the official container:

```bash
docker run --rm --user root --group-add dialout -e HOME=/tmp \
  --device=/dev/ttyACM0 \
  -v "$(git rev-parse --show-toplevel):/host" \
  -w /host/firmware/main-deck-jc1060/examples/esp_draw_bit \
  espressif/idf:v6.0.2 bash -lc \
  'source /opt/esp/idf/export.sh >/dev/null && idf.py -p /dev/ttyACM0 flash monitor'
```

(`root` + `--group-add dialout` is only needed to reach the host USB node
owned by `root:dialout`; `--device=/dev/ttyACM0` exposes it. Exit the monitor
with `ctrl+]`.)

## Expected Output

1. **Boot log** should show:
   ```
   I (xxx) bsp_board: Initializing board JC1060P470C_I_W_Y
   I (xxx) bsp_display: Display initialized: 1024x600 @ 60Hz
   I (xxx) bsp_touch: GT911 touch initialized
   I (xxx) bsp_audio: Audio initialized (if codec detected)
   ```

2. **Display** should show:
   - Color bars (8 vertical stripes)
   - Horizontal gradient
   - Vertical gradient
   - Solid red/green/blue screens

3. **Touch test**: Touch the screen and monitor serial output:
   ```
   I (xxx) draw_bit: Touch: (512, 300) strength=128
   ```
   White circles should appear at touch positions.

## Troubleshooting

### Display stays black
- Check `BSP_LCD_BL_GPIO` (GPIO23) - measure voltage
- Verify MIPI-DSI lane configuration (2 lanes @ 1Gbps)
- Check JD9165 init sequence timing

### Touch not responding
- Verify I2C pins (GPIO14/15 for software I2C)
- Check GT911 address (0x5D)
- Measure pull-up resistors on SDA/SCL

### Audio noise/static
- Check I2S pinout (GPIO13/12/9)
- Verify ES8311 I2C address (0x18)
- Test with different sample rates

## Hardware Modifications

If GPIO12 conflict persists (I2S LRCK vs I2C SDA):
- Option 1: Use I2S1 instead (GPIO14-17)
- Option 2: Hardware trace cut + jumper

## Next Steps

After successful validation:
1. Integrate BSP into main Pajoniiir firmware
2. Port LVGL UI from JC4880P443C
3. Test with DDJ-400 controller
