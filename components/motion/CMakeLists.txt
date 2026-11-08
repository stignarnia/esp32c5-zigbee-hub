idf_component_register(
    SRCS "motion.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_wifi esp_netif esp_timer log nvs_flash lwip wifi5g
)
