/*
 * Web-UI settings module — REST endpoints for runtime control of WireGuard
 * and Tailscale (enable/disable, credentials, peer config).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>

/**
 * Register settings endpoints on an existing HTTP server.
 * Endpoints:
 *   GET  /api/network/status
 *   GET  /api/tailscale
 *   PUT  /api/tailscale
 *   GET  /api/wireguard
 *   PUT  /api/wireguard
 */
esp_err_t web_settings_register(httpd_handle_t server);

/**
 * Read the per-service runtime enable flag from NVS, falling back to the
 * supplied compile-time default if the key is missing.
 *
 * Used by app_main() at boot to decide whether to start each VPN service.
 */
bool web_settings_get_enabled(const char *namespace_, bool default_value);
