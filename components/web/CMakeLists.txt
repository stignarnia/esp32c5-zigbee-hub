idf_component_register(
    SRCS "web_server.c" "web_api.c" "auth.c" "access.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_common
    PRIV_REQUIRES esp_http_server esp_timer esp_system esp_netif app_update esp_app_format
                  nvs_flash mbedtls log hubsys hublog wifi5g spi_flash zb tailscale
                  esp_driver_gpio
)

# UI assets: gzipped at build time (-n: no timestamp, reproducible), embedded as
# _binary_<name>_gz_start/_end and served with Content-Encoding: gzip.
find_program(GZIP gzip REQUIRED)
foreach(asset index.html app.js style.css)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/ui/${asset}")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${asset}.gz")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${GZIP} -9 -n -c "${src}" > "${out}"
        DEPENDS "${src}"
        VERBATIM)
    string(MAKE_C_IDENTIFIER "${asset}" id)
    add_custom_target(web_gz_${id} DEPENDS "${out}")
    target_add_binary_data(${COMPONENT_LIB} "${out}" BINARY DEPENDS web_gz_${id})
endforeach()
