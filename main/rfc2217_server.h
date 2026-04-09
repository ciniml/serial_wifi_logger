/*
 * SPDX-FileCopyrightText: 2024 Kenta IDA
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFC2217 Server API
 */

#ifndef RFC2217_SERVER_H
#define RFC2217_SERVER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start the RFC2217 server
 *
 * Starts a TCP server on the configured port (default 2217) that
 * implements the RFC2217 Telnet COM Port Control protocol.
 *
 * @return ESP_OK on success
 */
esp_err_t rfc2217_server_init(void);

/**
 * @brief Stop the RFC2217 server
 *
 * @return ESP_OK on success
 */
esp_err_t rfc2217_server_stop(void);

/**
 * @brief Check if an RFC2217 client is connected
 *
 * @return true if a client is connected
 */
bool rfc2217_server_is_connected(void);

/**
 * @brief Send data to connected RFC2217 client (from USB)
 *
 * Data will be escaped (IAC -> IAC IAC) before transmission.
 *
 * @param data Data buffer
 * @param len Data length
 * @return ESP_OK on success
 */
esp_err_t rfc2217_server_send_data(const uint8_t *data, size_t len);

/**
 * @brief Notify client of modem state change
 *
 * Sends NOTIFY-MODEMSTATE subnegotiation to the client.
 *
 * @param cts Clear To Send state
 * @param dsr Data Set Ready state
 * @param ri Ring Indicator state
 * @param cd Carrier Detect state
 * @return ESP_OK on success
 */
esp_err_t rfc2217_server_notify_modemstate(bool cts, bool dsr, bool ri, bool cd);

/**
 * @brief Notify client of line state change
 *
 * Sends NOTIFY-LINESTATE subnegotiation to the client.
 *
 * @param state Line state bits (RFC2217_LINESTATE_*)
 * @return ESP_OK on success
 */
esp_err_t rfc2217_server_notify_linestate(uint8_t state);

#ifdef __cplusplus
}
#endif

#endif // RFC2217_SERVER_H
