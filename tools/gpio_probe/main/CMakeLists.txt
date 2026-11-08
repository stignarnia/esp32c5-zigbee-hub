idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_adc esp_driver_gpio esp_driver_uart app_update log
                                     esp_wifi esp_netif esp_event nvs_flash lwip esp_driver_rmt)
