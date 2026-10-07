# v246-v249: Pioneer DJ Link observer, virtual CDJ, peer library browse and
# peer track download to the SD cache (Ethernet only). dj_link_state.c,
# dj_link_session.c, dj_link_master.c (v305), dj_link_db.c, dj_link_pdb.c and
# dj_link_anlz.c, dj_link_neighbor.c (v338), dj_link_clock.c (v339), dj_link_mixer.c (v341), dj_link_nfsd.c and dj_link_nfsd_fs.c (v342) are pure and host tested in test/host; dj_link.c is the
# lwIP/FreeRTOS/FATFS transport.
idf_component_register(SRCS "dj_link.c" "dj_link_state.c" "dj_link_session.c" "dj_link_master.c"
                            "dj_link_db.c" "dj_link_neighbor.c" "dj_link_clock.c" "dj_link_mixer.c"
                            "dj_link_nfsd.c" "dj_link_nfsd_fs.c" "dj_link_nfsd_service.c"
                            "dj_link_pdb.c" "dj_link_anlz.c"
                       INCLUDE_DIRS "include"
                       REQUIRES djlink esp_netif
                       PRIV_REQUIRES lwip esp_timer freertos log sd_io_gate sd_idle_wait vfs)
