/*
 * WireGuard ESP-IDF 6.0 integration layer
 * Ported from WireGuard-ESP32-Arduino by Kenta Ida (fuga@fugafuga.org)
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WireGuard tunnel configuration.
 *
 * All string pointers must remain valid for the lifetime of the tunnel
 * (until wireguard_esp32_stop() returns). When passing NULL to
 * wireguard_esp32_start(), the component loads config from NVS (namespace
 * "wireguard"), falling back to Kconfig defaults for any missing keys.
 */
typedef struct {
    const char    *local_ip;         /*!< Local VPN IPv4 address, e.g. "10.0.0.2" */
    const char    *local_netmask;    /*!< Local VPN netmask, e.g. "255.255.255.0" */
    const char    *local_gateway;    /*!< Local VPN gateway, e.g. "0.0.0.0" */
    const char    *private_key;      /*!< Base64-encoded 32-byte private key */
    const char    *peer_public_key;  /*!< Base64-encoded 32-byte peer public key */
    const char    *peer_endpoint;    /*!< Peer hostname or IP string */
    uint16_t       peer_port;        /*!< Peer UDP port */
    uint16_t       listen_port;      /*!< Local listen port (0 = ephemeral) */
    uint16_t       keepalive;        /*!< Persistent keepalive seconds; 0 = disabled */
    const uint8_t *preshared_key;    /*!< Optional 32-byte preshared key, or NULL */
    bool           set_as_default;   /*!< Set WireGuard netif as default route */
} wireguard_config_t;

/**
 * @brief Start the WireGuard tunnel.
 *
 * Resolves the peer endpoint, creates the WireGuard lwIP netif, adds the
 * peer, and initiates the handshake. Must be called after WiFi STA has
 * obtained an IP address.
 *
 * NTP synchronisation is strongly recommended before calling this function.
 * Without a correct system clock, WireGuard handshakes will be rejected by
 * the peer after a device reset (TAI64N timestamp replay protection).
 *
 * @param config  Tunnel parameters. Pass NULL to load from NVS + Kconfig.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t wireguard_esp32_start(const wireguard_config_t *config);

/**
 * @brief Stop and tear down the WireGuard tunnel.
 *
 * Safe to call even if the tunnel never connected or was never started.
 *
 * @return ESP_OK on success.
 */
esp_err_t wireguard_esp32_stop(void);

/**
 * @brief Query whether the WireGuard peer session is active.
 *
 * A peer is "up" when a handshake has completed successfully and the
 * session key has not yet expired.
 *
 * @return true if the peer is up, false otherwise.
 */
bool wireguard_esp32_is_peer_up(void);

#ifdef __cplusplus
}
#endif
