# Tailscale node: microlink (MIT, Cameron Malone) + wireguard-lwip (BSD-3, Daniel Hope),
# ported to IDF 6.1 / mbedTLS 4 / PSA. Upstream: github.com/CamM2325/microlink @ 216da33.
# Removed: cellular/PPP/AT sockets, network switching, config httpd, zero-copy WG,
# UDP/TCP socket API, NVS peer cache. See ts.c for the hub-side wrapper.
idf_component_register(
    SRCS "ts.c"
         "microlink/microlink.c"
         "microlink/ml_coord.c"
         "microlink/ml_derp.c"
         "microlink/ml_net_io.c"
         "microlink/ml_wg_mgr.c"
         "microlink/ml_stun.c"
         "microlink/ml_pmp.c"
         "microlink/ml_noise.c"
         "microlink/ml_h2.c"
         "microlink/nacl_box.c"
         "wireguard/wireguard.c"
         "wireguard/wireguardif.c"
         "wireguard/crypto.c"
         "wireguard/wireguard-platform-esp32.c"
         "wireguard/crypto/refc/blake2s.c"
         "wireguard/crypto/refc/chacha20.c"
         "wireguard/crypto/refc/chacha20poly1305.c"
         "wireguard/crypto/refc/poly1305-donna.c"
         "wireguard/crypto/refc/x25519.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "microlink" "wireguard" "wireguard/crypto/refc"
    REQUIRES esp_common
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_timer lwip mbedtls log
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE WIREGUARD_CRYPTO_REFC=1)
# Third-party code: keep its warnings visible but non-fatal.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
set_source_files_properties(wireguard/crypto/refc/x25519.c PROPERTIES COMPILE_FLAGS "-Wno-stringop-overread")

# lwIP input hook (hub_lwip_hooks.h): drop packets for the tailnet IP that arrive on
# Wi-Fi. lwIP includes the header from its options file, so the define and the
# include path must be on the lwip component itself.
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_include_directories(${lwip_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_definitions(${lwip_lib} PRIVATE "ESP_IDF_LWIP_HOOK_FILENAME=\"hub_lwip_hooks.h\"")
