# v246-v249: Pioneer DJ Link observer, virtual CDJ, peer library browse and
# peer track download to the SD cache (Ethernet only). dj_link_state.c,
# dj_link_session.c, dj_link_db.c and dj_link_pdb.c are pure and host tested
# in test/host; dj_link.c is the lwIP/FreeRTOS/FATFS transport.
idf_component_register(SRCS "dj_link.c" "dj_link_state.c" "dj_link_session.c" "dj_link_db.c"
                            "dj_link_pdb.c"
                       INCLUDE_DIRS "include"
                       REQUIRES djlink esp_netif
                       PRIV_REQUIRES lwip esp_timer freertos log sd_io_gate vfs)
