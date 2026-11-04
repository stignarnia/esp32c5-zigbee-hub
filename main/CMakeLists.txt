idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_psram spi_flash esp_partition app_update esp_system nvs_flash
                  prov wifi5g hubsys hublog web esp_timer
)
