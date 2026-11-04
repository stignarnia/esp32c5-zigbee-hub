idf_component_register(
    SRCS "hubsys.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES app_update esp_app_format bootloader_support esp_system nvs_flash log wifi5g
)
