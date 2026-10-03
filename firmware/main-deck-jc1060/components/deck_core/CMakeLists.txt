idf_component_register(
    SRCS "deck_core.c" "deck_loaded_track_store.c" "deck_cue_park.c" "deck_jog_scrub.c" "deck_hot_cue_recall.c" "deck_load_lock.c" "deck_net_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES control_link log freertos audio_engine hot_cue_store beat_jump library
)
