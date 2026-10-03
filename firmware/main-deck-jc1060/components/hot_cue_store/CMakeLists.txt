idf_component_register(
    SRCS "hot_cue_store.c" "hot_cue_store_merge.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash log
)
