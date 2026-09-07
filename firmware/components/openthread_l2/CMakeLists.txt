idf_component_register(
    SRCS
        "openthread_l2.c"
        "openthread_l2_util.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES openthread freertos
)
