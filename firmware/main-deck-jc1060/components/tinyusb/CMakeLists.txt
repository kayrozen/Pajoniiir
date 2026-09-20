idf_component_register(
    SRCS
        "tinyusb_src/tusb.c"
        "tinyusb_src/common/tusb_fifo.c"
        "tinyusb_src/host/usbh.c"
        "tinyusb_src/host/hub.c"
        "tinyusb_src/class/audio/audio_host.c"
        "tinyusb_src/class/midi/midi_host.c"
        "tinyusb_src/portable/synopsys/dwc2/hcd_dwc2.c"
        "tinyusb_src/portable/synopsys/dwc2/dwc2_common.c"
    INCLUDE_DIRS
        "tinyusb_src"
    REQUIRES
        driver
        esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CFG_TUSB_MCU=OPT_MCU_ESP32P4
)
