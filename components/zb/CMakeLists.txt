idf_component_register(
    SRCS "zb.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES log esp_timer esp_system esp_coex ieee802154 nvs_flash esp_partition
)
