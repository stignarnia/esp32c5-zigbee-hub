idf_component_register(
    SRCS "hublog.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES log esp_timer esp_system esp_netif vfs
)
