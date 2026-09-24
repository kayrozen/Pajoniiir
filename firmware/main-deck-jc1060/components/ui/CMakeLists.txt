idf_component_register(
    SRCS "ui.c"
         "splash_screen.c"
         "Musieer_80.c"
         "Musieer_48.c"
         "ui_library.c"
         "ui_load_gate.c"
         "ui_overview.c"
         "ui_lvgl_backend.c"
         "ui_active_deck_leds.c"
         "ui_controls.c"
         "ui_deck_anlz_store.c"
         "ui_event_counter.c"
         "ui_mixer_view.c"
         "ui_overview_motion.c"
         "ui_overview_window.c"
         "ui_overlay_map.c"
         "ui_overview_perf.c"
         "ui_overview_scheduler.c"
         "ui_overview_renderer.c"
         "ui_overview_wave_cache.c"
         "ui_overview_grid.c"
         "ui_performance_target.c"
         "ui_performance_tabs.c"
         "ui_position_interpolator.c"
         "ui_waveform_model.c"
         "ui_beat_indicator.c"
         "ui_beat_fx_format.c"
         "ui_settings.c"
         "ui_status.c"
         "ui_idle.c"
    INCLUDE_DIRS "include"
    REQUIRES log deck_core library bsp_jc4880 audio_engine audio_recorder service_log app_settings control_link firmware_health media_catalog hot_cue_store fatfs esp_lcd esp_lcd_touch esp_timer esp_driver_gpio esp_driver_ppa esp_mm idf::lvgl__lvgl
)

# JC1060P470C_I_W_Y: 1024x600 native landscape UI (same defines as the
# tests/ui_simulator_e2e_jc1060 gate).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    UI_TARGET_JC1060=1
    UI_HOR_RES=1024
    UI_VER_RES=600
    UI_TOPBAR_H=54
    UI_CONTENT_Y=54
    UI_CONTENT_H=546
)
