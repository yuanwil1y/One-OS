idf_component_register(
    SRCS
        "ha_mdns.c"
        "ha_mdns_parser.c"
        "ha_ssdp.c"
        "ha_ssdp_parser.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES lwip freertos
)
