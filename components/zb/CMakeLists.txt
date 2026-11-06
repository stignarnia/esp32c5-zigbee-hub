idf_component_register(
    SRCS "zb.c" "zb_dev.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common espressif__cjson
    PRIV_REQUIRES log esp_timer esp_system esp_coex ieee802154 nvs_flash esp_partition vfs
)
