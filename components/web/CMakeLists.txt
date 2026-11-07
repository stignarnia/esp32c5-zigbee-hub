idf_component_register(
    SRCS "web_server.c" "web_api.c" "web_auto.c" "auth.c" "access.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_common
    PRIV_REQUIRES esp_http_server esp_timer esp_system esp_netif app_update esp_app_format
                  nvs_flash mbedtls log hubsys hublog wifi5g spi_flash zb tailscale
                  esp_driver_gpio automation
)

# UI assets: gzipped at build time (-n: no timestamp, reproducible), embedded as
# _binary_<name>_gz_start/_end and served with Content-Encoding: gzip.
find_program(GZIP gzip REQUIRED)
idf_build_get_property(python PYTHON)
idf_build_get_property(project_dir PROJECT_DIR)

# app.js gets the example scripts (examples/*.lua) put in first, see gen_app_js.py.
file(GLOB example_scripts CONFIGURE_DEPENDS "${project_dir}/examples/*.lua")
set(app_js "${CMAKE_CURRENT_BINARY_DIR}/app.js")
add_custom_command(
    OUTPUT "${app_js}"
    COMMAND ${python} "${CMAKE_CURRENT_SOURCE_DIR}/gen_app_js.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/app.js" "${project_dir}/examples" "${app_js}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui/app.js" "${CMAKE_CURRENT_SOURCE_DIR}/gen_app_js.py" ${example_scripts}
    VERBATIM)

# cm.js is the script editor (CodeMirror), prebuilt by tools/codemirror/build.sh.
foreach(asset index.html app.js style.css cm.js)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/ui/${asset}")
    if(asset STREQUAL "app.js")
        set(src "${app_js}")
    endif()
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
