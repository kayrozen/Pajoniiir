idf_component_register(
    SRCS "ui.c"
         "splash_screen.c"
         "Musieer_80.c"
         "Musieer_48.c"
         "ui_library.c"
         "ui_load_gate.c"
         "ui_overview.c"
         "ui_lvgl_backend.c"
         "ui_controls.c"
         "ui_hot_cue_view.c"
         "ui_deck_anlz_store.c"
         "ui_event_counter.c"
         "ui_overview_motion.c"
         "ui_overview_window.c"
         "ui_overlay_map.c"
         "ui_overview_perf.c"
         "ui_overview_renderer.c"
         "ui_overview_wave_cache.c"
         "ui_overview_grid.c"
         "ui_performance_target.c"
         "ui_audible_position.c"
         "ui_position_interpolator.c"
         "ui_waveform_model.c"
         "ui_beat_indicator.c"
         "ui_beat_fx_format.c"
         "ui_settings.c"
         "ui_idle.c"
         "dj_ui.c"
         "ui_djui_bridge.c"
         "ui_djui_text.c"
         "ui_artwork.c"
         "ui_artwork_thumb.c"
    INCLUDE_DIRS "include"
    REQUIRES log deck_core library bsp_jc4880 audio_engine audio_recorder service_log app_settings control_link firmware_health media_catalog hot_cue_store controller_profile_manager dj_link media_io_gate fatfs esp_lcd esp_lcd_touch esp_timer esp_driver_gpio esp_driver_ppa esp_mm idf::lvgl__lvgl
)

# JC1060P470C_I_W_Y: 1024x600 native landscape UI (same size as the
# tests/ui_simulator_e2e_jc1060 gate). v293: UI_TOPBAR_H/UI_CONTENT_* are
# gone with the legacy widgets; dj_ui lays out its own 40 px tab row.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    UI_TARGET_JC1060=1
    UI_HOR_RES=1024
    UI_VER_RES=600
)
