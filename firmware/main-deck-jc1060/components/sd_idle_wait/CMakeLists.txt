# v321: yielding replacement for IDF's sdmmc_wait_for_idle() (see
# sd_idle_wait.c). The IDF 6.0.2 driver busy-polls CMD13 for up to 100 ms
# after every SD write (and every read that finds the card busy); on the P4
# that command storm slows both cores (esp-idf #19034, fixed on release/v6.0
# after v6.0.3). Remove this component once the tree moves to an IDF release
# carrying the fix (sdmmc_poll_delay_and_backoff in the IDF sdmmc component).
idf_component_register(
    SRCS "sd_idle_wait.c"
    INCLUDE_DIRS "include"
    REQUIRES sdmmc
    PRIV_REQUIRES esp_timer freertos
    WHOLE_ARCHIVE
)

target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=sdmmc_wait_for_idle")
