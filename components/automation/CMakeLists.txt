idf_component_register(
    SRCS "auto_engine.c" "auto_store.c" "auto_gpio.c" "auto_http.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_common espressif__cjson
    PRIV_REQUIRES lua zb wifi5g hublog log esp_timer esp_system nvs_flash vfs esp_driver_gpio
                  esp_driver_ledc esp_http_client mbedtls lwip freertos heap
)
