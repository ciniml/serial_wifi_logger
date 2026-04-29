# Only build WireGuard when enabled in Kconfig
# Pattern follows components/usb_host_ftdi_sio/CMakeLists.txt
if(CONFIG_WIREGUARD_ENABLE)
    file(GLOB REFC_SRCS "src/crypto/refc/*.c")

    # wireguard_esp32.h only exposes esp_err_t (from esp_common, always available).
    # All lwIP / esp_netif / mbedtls / nvs usage is internal → PRIV_REQUIRES.
    # Note: PRIV_REQUIRES handles link-time deps; include dirs are added explicitly
    # below via target_include_directories to work around MINIMAL_BUILD include
    # propagation in ESP-IDF 6.0 beta.
    #
    # src/ and src/crypto/refc/ are also listed in INCLUDE_DIRS (not just
    # PRIV_INCLUDE_DIRS) so that dependent components (e.g. tailscale) can
    # #include "crypto.h", "wireguard.h", "wireguardif.h" and the crypto
    # primitive headers (x25519.h, chacha20poly1305.h, blake2s.h) directly.
    idf_component_register(
        SRCS
            "src/wireguard.c"
            "src/wireguardif.c"
            "src/wireguard-platform.c"
            "src/crypto.c"
            ${REFC_SRCS}
            "src/wireguard_esp32.c"
        INCLUDE_DIRS
            "include"
            "src"
            "src/crypto/refc"
        PRIV_INCLUDE_DIRS
        PRIV_REQUIRES
            esp_netif
            lwip
            esp_wifi
            nvs_flash
            mbedtls
            esp_system
    )

    # Explicitly add include directories for components whose include paths are
    # not propagated via PRIV_REQUIRES in MINIMAL_BUILD mode (ESP-IDF 6.0 beta).
    idf_component_get_property(mbedtls_dir   mbedtls    COMPONENT_DIR)
    idf_component_get_property(esp_netif_dir esp_netif  COMPONENT_DIR)
    idf_component_get_property(nvs_flash_dir nvs_flash  COMPONENT_DIR)
    idf_component_get_property(esp_event_dir esp_event  COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        ${mbedtls_dir}/mbedtls/include
        ${mbedtls_dir}/port/include
        ${esp_netif_dir}/include
        ${nvs_flash_dir}/include
        ${esp_event_dir}/include
    )

    # Suppress GCC 13 false-positive on x25519.c: a24[1] is passed as const fe b
    # but the loop is guarded by nb=1 so only b[0] is ever read.
    set_source_files_properties(
        "src/crypto/refc/x25519.c"
        PROPERTIES COMPILE_FLAGS "-Wno-stringop-overread"
    )
else()
    # Register empty component when not selected
    idf_component_register()
endif()
