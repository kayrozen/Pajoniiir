# DJ Deck Display — LVGL v9 (1024×600)

Fichiers :
- `dj_ui.h` / `dj_ui.c` : l'interface (4 onglets) et l'API de mise à jour.
- `dj_ui_demo.c` : données factices et animation. Retirez ce fichier en production.

## lv_conf.h

```c
#define LV_COLOR_DEPTH 16
#define LV_USE_FLEX    1
#define LV_USE_BAR     1
#define LV_USE_SLIDER  1
#define LV_USE_SWITCH  1
#define LV_USE_IMAGE   1
#define LV_USE_LABEL   1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
```

Les polices Montserrat intégrées couvrent seulement l'ASCII. Pour les accents dans les titres de pistes, générez une police avec `lv_font_conv` (plage 0x20-0x17F) et remplacez les macros `F12`…`F32` en haut de `dj_ui.c`.

## Utilisation

```c
lv_init();
/* ... init display + touch driver ... */
dj_ui_create(lv_screen_active());
dj_ui_demo_start();           /* ou vos propres appels */
while (1) { lv_timer_handler(); usleep(5000); }
```

Avec des données réelles :

```c
static const dj_ui_callbacks_t cb = { .on_lib_load = my_load, .on_hotcue = my_cue, /* ... */ };
dj_ui_set_callbacks(&cb);

dj_ui_set_track(0, "Title", "Artist", "USB 1", 3, 128, 312000);
dj_ui_set_key(0, "8A");
dj_ui_set_bpm(0, 124.0f, first_beat_ms);
dj_ui_set_tempo(0, 1.2f);
dj_ui_set_waveform(0, peaks, cores, n_points, 100);   /* buffers non copiés */
dj_ui_set_position(0, pos_ms);                         /* ~30 fois/s */
dj_ui_set_hotcue(0, 0, true, 11000, 3);
dj_ui_set_field(DJ_F_SYS_CONTROLLER, "Controller (USB1): Disconnected", DJ_TONE_ERROR);
```

Si vos données arrivent depuis une autre tâche (MIDI, réseau), entourez les appels de `lv_lock()` / `lv_unlock()`.

## Notes

- Waveform zoomée : 8 s visibles (`ZOOM_WINDOW_MS`). Au moins 100 points/s donnent un rendu net.
- Les deux waveforms zoomées sont redessinées à chaque `dj_ui_set_position`. Sur ESP32-S3, limitez la fréquence à 20-25 Hz. Sur ESP32-P4, 30 Hz ne pose pas de problème.
- La pochette doit faire 34×34 px (`lv_image_dsc_t` ou fichier via le driver FS).
- La tonalité (`dj_ui_set_key`) met aussi à jour l'étiquette « BPM / 8A » du pied de deck.
