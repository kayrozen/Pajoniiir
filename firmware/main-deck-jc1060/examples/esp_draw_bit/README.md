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

```bash
cd firmware/main-deck-jc1060/examples/esp_draw_bit
idf.py set-target esp32p4
idf.py build
```

## Flashing

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Or on Windows:
```powershell
idf.py -p COM15 flash monitor
```

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
