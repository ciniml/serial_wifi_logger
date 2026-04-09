/*
 * SPDX-FileCopyrightText: 2024 Kenta IDA
 * SPDX-License-Identifier: Apache-2.0
 *
 * Serial Control Abstraction Layer
 *
 * Provides unified API for controlling serial port settings
 * on both CDC-ACM and FTDI USB-serial devices.
 */

#ifndef SERIAL_CONTROL_H
#define SERIAL_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Line coding structure (compatible with CDC-ACM)
// ============================================================================

typedef struct {
    uint32_t baudrate;      // Baudrate in bps
    uint8_t data_bits;      // Data bits: 5, 6, 7, 8
    uint8_t parity;         // Parity: 0=None, 1=Odd, 2=Even, 3=Mark, 4=Space
    uint8_t stop_bits;      // Stop bits: 0=1, 1=1.5, 2=2
} serial_line_coding_t;

// ============================================================================
// Modem status structure
// ============================================================================

typedef struct {
    bool cts;               // Clear To Send
    bool dsr;               // Data Set Ready
    bool ri;                // Ring Indicator
    bool cd;                // Carrier Detect (RLSD)
} serial_modem_status_t;

// ============================================================================
// Function prototypes
// ============================================================================

/**
 * @brief Check if a serial device is currently connected
 * @return true if device is connected and ready
 */
bool serial_control_is_connected(void);

/**
 * @brief Set serial line coding (baudrate, data bits, parity, stop bits)
 * @param coding Pointer to line coding structure
 * @return ESP_OK on success
 */
esp_err_t serial_control_set_line_coding(const serial_line_coding_t *coding);

/**
 * @brief Get current serial line coding
 * @param coding Pointer to structure to receive current settings
 * @return ESP_OK on success
 */
esp_err_t serial_control_get_line_coding(serial_line_coding_t *coding);

/**
 * @brief Set baudrate only
 * @param baudrate Baudrate in bps
 * @return ESP_OK on success
 */
esp_err_t serial_control_set_baudrate(uint32_t baudrate);

/**
 * @brief Get current baudrate
 * @param baudrate Pointer to receive current baudrate
 * @return ESP_OK on success
 */
esp_err_t serial_control_get_baudrate(uint32_t *baudrate);

/**
 * @brief Set DTR (Data Terminal Ready) signal
 * @param state true=ON, false=OFF
 * @return ESP_OK on success
 */
esp_err_t serial_control_set_dtr(bool state);

/**
 * @brief Get current DTR state
 * @param state Pointer to receive current state
 * @return ESP_OK on success
 */
esp_err_t serial_control_get_dtr(bool *state);

/**
 * @brief Set RTS (Request To Send) signal
 * @param state true=ON, false=OFF
 * @return ESP_OK on success
 */
esp_err_t serial_control_set_rts(bool state);

/**
 * @brief Get current RTS state
 * @param state Pointer to receive current state
 * @return ESP_OK on success
 */
esp_err_t serial_control_get_rts(bool *state);

/**
 * @brief Set both DTR and RTS signals
 * @param dtr DTR state
 * @param rts RTS state
 * @return ESP_OK on success
 */
esp_err_t serial_control_set_modem_control(bool dtr, bool rts);

/**
 * @brief Get modem status (CTS, DSR, RI, CD)
 *
 * Note: This is only supported on FTDI devices.
 * On CDC-ACM devices, this will return all signals as false.
 *
 * @param status Pointer to structure to receive modem status
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if device doesn't support
 */
esp_err_t serial_control_get_modem_status(serial_modem_status_t *status);

/**
 * @brief Send break signal
 * @param on true to start break, false to stop
 * @return ESP_OK on success
 */
esp_err_t serial_control_set_break(bool on);

/**
 * @brief Transmit data
 * @param data Data buffer
 * @param len Data length
 * @param timeout_ms Timeout in milliseconds
 * @return ESP_OK on success
 */
esp_err_t serial_control_transmit(const uint8_t *data, size_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // SERIAL_CONTROL_H
