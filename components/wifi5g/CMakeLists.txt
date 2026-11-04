idf_component_register(
    SRCS "wifi5g.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer esp_system log nvs_flash
)
