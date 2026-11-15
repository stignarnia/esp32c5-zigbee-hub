# Home Assistant integration: ESPHome native API server (Noise), see esphome_api.h.
idf_component_register(
    SRCS "ha_server.c" "ha_model.c" "ha_noise.c" "ha_pb.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_common automation zb
    PRIV_REQUIRES hubcrypto mbedtls lwip nvs_flash esp_timer esp_app_format esp_hw_support log freertos heap
                  hublog hubsys motion tailscale wifi5g espressif__mdns
)
