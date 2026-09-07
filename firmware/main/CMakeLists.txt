idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        board
        lvgl_port
        lvgl
        freertos
        ha_core
        ha_discovery_l2
        esphome_l2
        theengs_l2
        zha_l2
        zigpy_l2
        openthread_l2
        kismet_l2
        nmap_l2
        wireshark_l2
        matter_l2
)
