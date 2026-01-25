#ifndef OTA_SERVER_H
#define OTA_SERVER_H

#include "esp_err.h"

/**
 * @brief Initialize and start the OTA HTTP server
 *
 * This function starts an HTTP server on the configured port
 * and registers handlers for the OTA update endpoints:
 * - GET  /              : Serve web UI for firmware upload
 * - GET  /api/info      : Return device information (JSON)
 * - POST /api/ota       : Handle firmware upload
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ota_server_init(void);

/**
 * @brief Stop the OTA HTTP server
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ota_server_stop(void);

#endif // OTA_SERVER_H
