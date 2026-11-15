# X25519 (STROBE reference code, MIT: x25519-license.txt; came with wireguard-lwip /
# microlink) and ChaCha20-Poly1305 via PSA, shared by Tailscale and the Home Assistant API.
idf_component_register(
    SRCS "hub_aead.c" "x25519.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mbedtls
)
set_source_files_properties(x25519.c PROPERTIES COMPILE_FLAGS "-Wno-stringop-overread -Wno-error")
