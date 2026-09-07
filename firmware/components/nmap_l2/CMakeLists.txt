idf_component_register(
    SRCS
        "nmap_core.c"
        "nmap_service.c"
        "nmap_runtime.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES lwip esp_netif freertos
)
