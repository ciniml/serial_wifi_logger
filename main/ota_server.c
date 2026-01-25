#include "ota_server.h"
#include "ota_web_ui.h"
#include "version.h"

#include <string.h>
#include <sys/param.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota_server";

// HTTP server handle
static httpd_handle_t server = NULL;

// OTA configuration
#define OTA_BUF_SIZE 4096

/**
 * @brief Handler for GET /
 * Serves the OTA web UI
 */
static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, ota_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief Handler for GET /api/info
 * Returns device information as JSON
 */
static esp_err_t handler_api_info(httpd_req_t *req)
{
    // Get running partition info
    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *partition_label = running ? running->label : "unknown";

    // Get uptime in seconds
    uint32_t uptime_sec = esp_log_timestamp() / 1000;

    // Get version string
    const char *version = get_version_string();

    // Build JSON response
    char json_response[256];
    snprintf(json_response, sizeof(json_response),
             "{\"version\":\"%s\",\"partition\":\"%s\",\"uptime\":%lu}",
             version, partition_label, (unsigned long)uptime_sec);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/ota
 * Receives and flashes firmware update
 */
static esp_err_t handler_api_ota(httpd_req_t *req)
{
    esp_err_t err;
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = NULL;
    uint8_t *ota_write_data = NULL;
    int binary_file_length = 0;
    bool is_ota_started = false;

    ESP_LOGI(TAG, "Starting OTA update, content length: %d bytes", req->content_len);

    // Validate content length
    if (req->content_len <= 0) {
        ESP_LOGE(TAG, "No content received");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware data");
        return ESP_FAIL;
    }

    if (req->content_len > CONFIG_OTA_MAX_UPLOAD_SIZE) {
        ESP_LOGE(TAG, "Firmware too large: %d bytes (max: %d)",
                 req->content_len, CONFIG_OTA_MAX_UPLOAD_SIZE);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware too large");
        return ESP_FAIL;
    }

    // Get the next OTA partition
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find update partition");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                           "No OTA partition available");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Writing to partition: %s (offset 0x%08lx, size 0x%08lx)",
             update_partition->label,
             (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    // Allocate buffer for receiving data
    ota_write_data = malloc(OTA_BUF_SIZE);
    if (ota_write_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate OTA buffer");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int total_received = 0;

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, (char *)ota_write_data,
                                       MIN(remaining, OTA_BUF_SIZE));

        if (recv_len < 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "Socket timeout, retrying...");
                continue;
            }
            ESP_LOGE(TAG, "Failed to receive firmware data");
            if (is_ota_started) {
                esp_ota_abort(update_handle);
            }
            free(ota_write_data);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Connection error");
            return ESP_FAIL;
        }

        if (recv_len == 0) {
            ESP_LOGW(TAG, "Connection closed by client");
            if (is_ota_started) {
                esp_ota_abort(update_handle);
            }
            free(ota_write_data);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Connection closed");
            return ESP_FAIL;
        }

        // Validate first chunk - check for ESP32 app magic byte
        if (total_received == 0) {
            if (ota_write_data[0] != ESP_IMAGE_HEADER_MAGIC) {
                ESP_LOGE(TAG, "Invalid firmware format (magic byte: 0x%02X, expected: 0x%02X)",
                         ota_write_data[0], ESP_IMAGE_HEADER_MAGIC);
                free(ota_write_data);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Invalid firmware format");
                return ESP_FAIL;
            }

            // Begin OTA update
            err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES,
                               &update_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
                free(ota_write_data);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "OTA begin failed");
                return ESP_FAIL;
            }
            is_ota_started = true;
            ESP_LOGI(TAG, "OTA started successfully");
        }

        // Write data to flash
        err = esp_ota_write(update_handle, (const void *)ota_write_data, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(update_handle);
            free(ota_write_data);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Flash write failed");
            return ESP_FAIL;
        }

        total_received += recv_len;
        remaining -= recv_len;
        binary_file_length += recv_len;

        // Log progress every 64KB
        if (total_received % (64 * 1024) == 0 || remaining == 0) {
            ESP_LOGI(TAG, "Written %d / %d bytes (%.1f%%)",
                     total_received, req->content_len,
                     (float)total_received / req->content_len * 100.0);
        }
    }

    ESP_LOGI(TAG, "Total binary data length: %d bytes", binary_file_length);

    // End OTA update
    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Firmware validation failed (checksum error)");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Firmware validation failed");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "OTA end failed");
        }
        free(ota_write_data);
        return ESP_FAIL;
    }

    // Set boot partition to the newly uploaded firmware
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
                 esp_err_to_name(err));
        free(ota_write_data);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                           "Failed to set boot partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update completed successfully");
    ESP_LOGI(TAG, "Next boot partition: %s", update_partition->label);

    // Send success response
    httpd_resp_sendstr(req, "OK");

    free(ota_write_data);

    // Schedule restart after a short delay
    ESP_LOGI(TAG, "Restarting in 3 seconds...");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();

    return ESP_OK;
}

/**
 * @brief Start the OTA HTTP server
 */
esp_err_t ota_server_init(void)
{
    if (server != NULL) {
        ESP_LOGW(TAG, "HTTP server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_OTA_HTTP_SERVER_PORT;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 4;
    config.stack_size = 6144;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    // Register URI handlers
    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handler_root,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_root);

    httpd_uri_t uri_api_info = {
        .uri = "/api/info",
        .method = HTTP_GET,
        .handler = handler_api_info,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_api_info);

    httpd_uri_t uri_api_ota = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = handler_api_ota,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_api_ota);

    ESP_LOGI(TAG, "HTTP server started successfully");
    ESP_LOGI(TAG, "OTA web UI available at: http://<device-ip>:%d/",
             config.server_port);

    return ESP_OK;
}

/**
 * @brief Stop the OTA HTTP server
 */
esp_err_t ota_server_stop(void)
{
    if (server == NULL) {
        ESP_LOGW(TAG, "HTTP server not running");
        return ESP_OK;
    }

    esp_err_t err = httpd_stop(server);
    if (err == ESP_OK) {
        server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    } else {
        ESP_LOGE(TAG, "Failed to stop HTTP server: %s", esp_err_to_name(err));
    }

    return err;
}
