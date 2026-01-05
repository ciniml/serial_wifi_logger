/*
 * SPDX-FileCopyrightText: 2026 Kenta Ida
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * USB Serial Auto-Detection Example
 *
 * This example demonstrates automatic driver selection between CDC-ACM and FTDI
 * based on the connected USB device's VID/PID.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usb/ftdi_sio_host.h"
#include "usb/ftdi_sio_host_ops.h"
#include "usb/ftdi_host_types.h"

// WiFi and TCP includes
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "esp_netif.h"

// Provisioning
#include "provisioning.h"

#define EXAMPLE_USB_HOST_PRIORITY   (20)
#define EXAMPLE_TX_STRING           ("Auto-detect test string!")
#define EXAMPLE_TX_TIMEOUT_MS       (1000)

static const char *TAG = "USB-AUTO";

// ============= TYPE DEFINITIONS =============

typedef enum {
    DEVICE_TYPE_UNKNOWN = 0,
    DEVICE_TYPE_CDC,
    DEVICE_TYPE_FTDI
} device_type_t;

typedef enum {
    DEVICE_STATE_DETECTED = 0,
    DEVICE_STATE_OPENING,
    DEVICE_STATE_OPEN,
    DEVICE_STATE_ERROR,
    DEVICE_STATE_DISCONNECTED
} device_state_t;

typedef struct {
    device_type_t type;
    device_state_t state;
    uint16_t vid;
    uint16_t pid;
    union {
        cdc_acm_dev_hdl_t cdc_hdl;
        ftdi_sio_dev_hdl_t ftdi_hdl;
    } handle;
    SemaphoreHandle_t disconnected_sem;
} device_info_t;

// TCP server management structure
typedef struct {
    int listen_sock;           // Listening socket
    int client_sock;           // Client socket (-1 = not connected)
    bool connected;            // Connection status
    SemaphoreHandle_t tx_mutex; // TX mutual exclusion
} tcp_server_t;

// Data buffer for queue items
typedef struct {
    uint8_t data[512];         // Data buffer
    size_t len;                // Data length
    bool in_use;               // Buffer in use flag
} data_buffer_t;

// Buffer pool management
typedef struct {
    data_buffer_t buffers[CONFIG_DATA_BUFFER_POOL_SIZE];
    SemaphoreHandle_t mutex;
} buffer_pool_t;

// ============= GLOBAL VARIABLES =============

static QueueHandle_t device_queue;
static tcp_server_t tcp_server;
static QueueHandle_t usb_to_tcp_queue;  // USB → TCP queue (stores buffer pointers)
static QueueHandle_t tcp_to_usb_queue;  // TCP → USB queue (stores buffer pointers)
static device_info_t *current_device = NULL;  // Currently connected USB device
static EventGroupHandle_t wifi_event_group;
static int s_retry_num = 0;
static buffer_pool_t buffer_pool;  // Static buffer pool

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define PROV_SUCCESS_BIT   BIT2
#define PROV_FAIL_BIT      BIT3

// ============= FORWARD DECLARATIONS =============

// USB functions
static void usb_lib_task(void *arg);
static void cdc_new_device_callback(usb_device_handle_t usb_dev);
static void ftdi_new_device_callback(uint16_t vid, uint16_t pid, void *user_arg);
static void cdc_handle_rx(uint8_t *data, size_t data_len, void *arg);
static void ftdi_handle_rx(const uint8_t *data, size_t data_len, void *arg);
static void cdc_handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx);
static void ftdi_handle_event(ftdi_sio_host_dev_event_t event, void *user_ctx);
static void handle_cdc_device(device_info_t *dev_info);
static void handle_ftdi_device(device_info_t *dev_info);
static void handle_device(device_info_t *dev_info);

// Buffer pool management functions
static void buffer_pool_init(void);
static data_buffer_t* buffer_alloc(void);
static void buffer_free(data_buffer_t *buf);

// WiFi and TCP functions
static void tcp_server_task(void *pvParameters);
static void usb_to_tcp_bridge_task(void *pvParameters);
static void tcp_to_usb_bridge_task(void *pvParameters);

// ============= USB HOST TASK =============

/**
 * @brief USB Host library handling task
 *
 * @param arg Unused
 */
static void usb_lib_task(void *arg)
{
    while (1) {
        // Start handling system events
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: All devices freed");
            // Continue handling USB events to allow device reconnection
        }
    }
}

// ============= BUFFER POOL MANAGEMENT =============

/**
 * @brief Initialize buffer pool
 */
static void buffer_pool_init(void)
{
    buffer_pool.mutex = xSemaphoreCreateMutex();
    if (buffer_pool.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create buffer pool mutex");
        return;
    }

    // Initialize all buffers as free
    for (int i = 0; i < CONFIG_DATA_BUFFER_POOL_SIZE; i++) {
        buffer_pool.buffers[i].in_use = false;
        buffer_pool.buffers[i].len = 0;
    }

    ESP_LOGI(TAG, "Buffer pool initialized with %d buffers", CONFIG_DATA_BUFFER_POOL_SIZE);
}

/**
 * @brief Allocate a buffer from the pool
 *
 * @return Pointer to allocated buffer, or NULL if pool is full
 */
static data_buffer_t* buffer_alloc(void)
{
    data_buffer_t *buf = NULL;

    xSemaphoreTake(buffer_pool.mutex, portMAX_DELAY);

    // Find first free buffer
    for (int i = 0; i < CONFIG_DATA_BUFFER_POOL_SIZE; i++) {
        if (!buffer_pool.buffers[i].in_use) {
            buffer_pool.buffers[i].in_use = true;
            buf = &buffer_pool.buffers[i];
            break;
        }
    }

    xSemaphoreGive(buffer_pool.mutex);

    if (buf == NULL) {
        ESP_LOGW(TAG, "Buffer pool exhausted");
    }

    return buf;
}

/**
 * @brief Free a buffer back to the pool
 *
 * @param buf Pointer to buffer to free
 */
static void buffer_free(data_buffer_t *buf)
{
    if (buf == NULL) {
        return;
    }

    xSemaphoreTake(buffer_pool.mutex, portMAX_DELAY);

    // Verify buffer is from our pool
    if (buf >= buffer_pool.buffers && buf < buffer_pool.buffers + CONFIG_DATA_BUFFER_POOL_SIZE) {
        buf->in_use = false;
        buf->len = 0;
    } else {
        ESP_LOGE(TAG, "Attempted to free buffer not from pool");
    }

    xSemaphoreGive(buffer_pool.mutex);
}

// ============= WIFI INITIALIZATION =============

// ============= WIFI EVENT HANDLER =============

/**
 * @brief WiFi event handler for STA connection
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < CONFIG_WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to the AP");
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "Connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ============= TCP SERVER AND BRIDGE TASKS =============

/**
 * @brief TCP server task
 *
 * Listens for TCP connections and handles client communication
 */
static void tcp_server_task(void *pvParameters)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(CONFIG_TCP_SERVER_PORT);

    // Create listening socket
    tcp_server.listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (tcp_server.listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(tcp_server.listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    ESP_LOGI(TAG, "TCP server binding to port %d", CONFIG_TCP_SERVER_PORT);
    int err = bind(tcp_server.listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(tcp_server.listen_sock);
        vTaskDelete(NULL);
        return;
    }

    err = listen(tcp_server.listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Socket listen failed: errno %d", errno);
        close(tcp_server.listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP server listening on port %d", CONFIG_TCP_SERVER_PORT);

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);

        // Accept new client connection
        int sock = accept(tcp_server.listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            continue;
        }

        // If already connected, close old connection
        if (tcp_server.connected && tcp_server.client_sock >= 0) {
            ESP_LOGI(TAG, "New client connecting, closing existing connection");
            shutdown(tcp_server.client_sock, SHUT_RDWR);
            close(tcp_server.client_sock);
        }

        // Set up new connection
        tcp_server.client_sock = sock;
        tcp_server.connected = true;

        char addr_str[16];
        inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "TCP client connected from %s:%d",
                 addr_str, ntohs(source_addr.sin_port));

        // Receive loop
        uint8_t rx_buffer[CONFIG_TCP_RX_BUFFER_SIZE];
        while (tcp_server.connected) {
            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);

            if (len < 0) {
                ESP_LOGE(TAG, "TCP recv failed: errno %d", errno);
                break;
            } else if (len == 0) {
                ESP_LOGI(TAG, "TCP client disconnected");
                break;
            } else {
                // Allocate buffer from pool
                data_buffer_t *buf = buffer_alloc();
                if (buf != NULL) {
                    memcpy(buf->data, rx_buffer, len);
                    buf->len = len;

                    // Send buffer pointer to queue
                    if (xQueueSend(tcp_to_usb_queue, &buf, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "TCP→USB queue full, data dropped");
                        buffer_free(buf);  // Free buffer if queue is full
                    }
                } else {
                    ESP_LOGW(TAG, "No buffer available, TCP data dropped");
                }
            }
        }

        // Connection closed
        shutdown(sock, SHUT_RDWR);
        close(sock);
        tcp_server.client_sock = -1;
        tcp_server.connected = false;
        ESP_LOGI(TAG, "TCP connection closed");
    }
}

/**
 * @brief USB → TCP bridge task
 *
 * Forwards data from USB to TCP client
 */
static void usb_to_tcp_bridge_task(void *pvParameters)
{
    data_buffer_t *buf;

    while (1) {
        if (xQueueReceive(usb_to_tcp_queue, &buf, portMAX_DELAY) == pdTRUE) {
            if (tcp_server.connected && tcp_server.client_sock >= 0) {
                xSemaphoreTake(tcp_server.tx_mutex, portMAX_DELAY);

                int to_write = buf->len;
                int written = 0;
                while (to_write > 0) {
                    int ret = send(tcp_server.client_sock, buf->data + written, to_write, 0);
                    if (ret < 0) {
                        ESP_LOGE(TAG, "TCP send failed: errno %d", errno);
                        tcp_server.connected = false;
                        break;
                    }
                    written += ret;
                    to_write -= ret;
                }

                xSemaphoreGive(tcp_server.tx_mutex);
            }

            // Free buffer after processing
            buffer_free(buf);
        }
    }
}

/**
 * @brief TCP → USB bridge task
 *
 * Forwards data from TCP to USB serial device
 */
static void tcp_to_usb_bridge_task(void *pvParameters)
{
    data_buffer_t *buf;

    while (1) {
        if (xQueueReceive(tcp_to_usb_queue, &buf, portMAX_DELAY) == pdTRUE) {
            if (current_device != NULL && current_device->state == DEVICE_STATE_OPEN) {
                esp_err_t err;

                if (current_device->type == DEVICE_TYPE_CDC) {
                    err = cdc_acm_host_data_tx_blocking(current_device->handle.cdc_hdl,
                                                         buf->data, buf->len, 1000);
                } else if (current_device->type == DEVICE_TYPE_FTDI) {
                    err = ftdi_sio_host_data_tx_blocking(current_device->handle.ftdi_hdl,
                                                          buf->data, buf->len, 1000);
                } else {
                    buffer_free(buf);
                    continue;
                }

                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "USB TX failed: %s", esp_err_to_name(err));
                }
            }

            // Free buffer after processing
            buffer_free(buf);
        }
    }
}

// ============= NEW DEVICE CALLBACKS =============

/**
 * @brief CDC-ACM new device callback
 *
 * Called when a new USB device is detected. Filters out FTDI devices
 * and adds CDC devices to the queue for handling.
 *
 * @param usb_dev USB device handle
 */
static void cdc_new_device_callback(usb_device_handle_t usb_dev)
{
    const usb_device_desc_t *desc;
    if (usb_host_get_device_descriptor(usb_dev, &desc) != ESP_OK) {
        return;
    }

    // Skip FTDI devices (let FTDI driver handle them)
    if (desc->idVendor == FTDI_VID) {
        ESP_LOGD(TAG, "Detected FTDI device (VID=0x%04X), skipping in CDC handler", FTDI_VID);
        return;
    }

    // Add to queue for CDC handling
    device_info_t dev_info = {
        .type = DEVICE_TYPE_CDC,
        .state = DEVICE_STATE_DETECTED,
        .vid = desc->idVendor,
        .pid = desc->idProduct
    };

    ESP_LOGI(TAG, "CDC device detected: VID=0x%04X PID=0x%04X", dev_info.vid, dev_info.pid);
    xQueueSend(device_queue, &dev_info, 0);
}

/**
 * @brief FTDI new device callback
 *
 * Called when a new FTDI device is detected (VID filtering already done by FTDI driver).
 * Adds FTDI devices to the queue for handling.
 *
 * @param vid Vendor ID
 * @param pid Product ID
 * @param user_arg User argument (unused)
 */
static void ftdi_new_device_callback(uint16_t vid, uint16_t pid, void *user_arg)
{
    // FTDI driver already filters by VID, so we can trust this is FTDI
    device_info_t dev_info = {
        .type = DEVICE_TYPE_FTDI,
        .state = DEVICE_STATE_DETECTED,
        .vid = vid,
        .pid = pid
    };
    
    ESP_LOGI(TAG, "FTDI device detected: VID=0x%04X PID=0x%04X", vid, pid);
    xQueueSend(device_queue, &dev_info, 0);
}

// ============= DATA/EVENT CALLBACKS =============

/**
 * @brief CDC-ACM data received callback
 *
 * @param data Pointer to received data
 * @param data_len Length of received data in bytes
 * @param arg Argument we passed to the device open function (device_info_t*)
 * @return true: data processed, false: expect more data
 */
static void cdc_handle_rx(uint8_t *data, size_t data_len, void *arg)
{
    ESP_LOGI(TAG, "[CDC] Data received (%d bytes)", data_len);
    //ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_len, ESP_LOG_INFO);

    // Forward data to TCP queue
    if (usb_to_tcp_queue != NULL) {
        size_t remaining = data_len;
        size_t offset = 0;

        while (remaining > 0) {
            // Allocate buffer from pool
            data_buffer_t *buf = buffer_alloc();
            if (buf == NULL) {
                ESP_LOGW(TAG, "[CDC] No buffer available, data dropped");
                break;
            }

            size_t chunk_size = remaining > sizeof(buf->data) ? sizeof(buf->data) : remaining;
            memcpy(buf->data, data + offset, chunk_size);
            buf->len = chunk_size;

            // Send buffer pointer to queue
            if (xQueueSend(usb_to_tcp_queue, &buf, 0) != pdTRUE) {
                ESP_LOGW(TAG, "[CDC] USB→TCP queue full, data dropped");
                buffer_free(buf);  // Free buffer if queue is full
                break;
            }

            offset += chunk_size;
            remaining -= chunk_size;
        }
    }

}

/**
 * @brief FTDI data received callback
 *
 * @param data Pointer to received data
 * @param data_len Length of received data in bytes
 * @param arg Argument we passed to the device open function (device_info_t*)
 */
static void ftdi_handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
    ESP_LOGI(TAG, "[FTDI] Data received (%d bytes)", data_len);
    //ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_len, ESP_LOG_INFO);

    // Forward data to TCP queue
    if (usb_to_tcp_queue != NULL) {
        size_t remaining = data_len;
        size_t offset = 0;

        while (remaining > 0) {
            // Allocate buffer from pool
            data_buffer_t *buf = buffer_alloc();
            if (buf == NULL) {
                ESP_LOGW(TAG, "[FTDI] No buffer available, data dropped");
                break;
            }

            size_t chunk_size = remaining > sizeof(buf->data) ? sizeof(buf->data) : remaining;
            memcpy(buf->data, data + offset, chunk_size);
            buf->len = chunk_size;

            // Send buffer pointer to queue
            if (xQueueSend(usb_to_tcp_queue, &buf, 0) != pdTRUE) {
                ESP_LOGW(TAG, "[FTDI] USB→TCP queue full, data dropped");
                buffer_free(buf);  // Free buffer if queue is full
                break;
            }

            offset += chunk_size;
            remaining -= chunk_size;
        }
    }
}

/**
 * @brief CDC-ACM event callback
 *
 * @param event Device event type and data
 * @param user_ctx Argument we passed to the device open function (device_info_t*)
 */
static void cdc_handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    device_info_t *dev_info = (device_info_t *)user_ctx;

    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "[CDC] Error: %i", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "[CDC] Device disconnected");
        cdc_acm_host_close(event->data.cdc_hdl);
        xSemaphoreGive(dev_info->disconnected_sem);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "[CDC] Serial state: 0x%04X", event->data.serial_state.val);
        break;
    case CDC_ACM_HOST_NETWORK_CONNECTION:
    default:
        ESP_LOGW(TAG, "[CDC] Unsupported event: %i", event->type);
        break;
    }
}

/**
 * @brief FTDI event callback
 *
 * @param event Device event type
 * @param user_ctx Argument we passed to the device open function (device_info_t*)
 */
static void ftdi_handle_event(ftdi_sio_host_dev_event_t event, void *user_ctx)
{
    device_info_t *dev_info = (device_info_t *)user_ctx;

    switch (event) {
    case FTDI_SIO_HOST_ERROR:
        ESP_LOGE(TAG, "[FTDI] Error occurred");
        break;
    case FTDI_SIO_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "[FTDI] Device disconnected");
        if (dev_info->handle.ftdi_hdl != NULL) {
            ftdi_sio_host_close(dev_info->handle.ftdi_hdl);
        }
        xSemaphoreGive(dev_info->disconnected_sem);
        break;
    case FTDI_SIO_HOST_MODEM_STATUS:
        ESP_LOGI(TAG, "[FTDI] Modem status changed");
        if (dev_info->handle.ftdi_hdl != NULL) {
            ftdi_modem_status_t status;
            if (ftdi_sio_host_get_modem_status(dev_info->handle.ftdi_hdl, &status) == ESP_OK) {
                ESP_LOGI(TAG, "[FTDI] Modem status: CTS=%d DSR=%d RI=%d CD=%d",
                         status.cts, status.dsr, status.ri, status.rlsd);
            }
        }
        break;
    default:
        ESP_LOGW(TAG, "[FTDI] Unsupported event: %i", event);
        break;
    }
}

// ============= DEVICE HANDLERS =============

/**
 * @brief Handle CDC-ACM device
 *
 * Opens and operates a CDC-ACM device based on usb_cdc_example_main.c
 *
 * @param dev_info Device information structure
 */
static void handle_cdc_device(device_info_t *dev_info)
{
    ESP_LOGI(TAG, "Opening CDC-ACM device (VID=0x%04X, PID=0x%04X)", dev_info->vid, dev_info->pid);

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 1000,
        .out_buffer_size = 512,
        .user_arg = dev_info,  // Pass dev_info for callback access
        .event_cb = cdc_handle_event,
        .data_cb = cdc_handle_rx
    };

    esp_err_t err = cdc_acm_host_open(dev_info->vid, dev_info->pid, 0, &dev_config, &dev_info->handle.cdc_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open CDC device: %s", esp_err_to_name(err));
        return;
    }

    dev_info->state = DEVICE_STATE_OPEN;
    ESP_LOGI(TAG, "[CDC] Device opened successfully");

    // Print device descriptor
    cdc_acm_host_desc_print(dev_info->handle.cdc_hdl);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Test sending data
    ESP_ERROR_CHECK(cdc_acm_host_data_tx_blocking(dev_info->handle.cdc_hdl,
                                                   (const uint8_t *)EXAMPLE_TX_STRING,
                                                   strlen(EXAMPLE_TX_STRING),
                                                   EXAMPLE_TX_TIMEOUT_MS));
    vTaskDelay(pdMS_TO_TICKS(100));

    // Test line coding: Get current, change to 9600 7N1, and read again
    ESP_LOGI(TAG, "[CDC] Setting up line coding");

    cdc_acm_line_coding_t line_coding;
    ESP_ERROR_CHECK(cdc_acm_host_line_coding_get(dev_info->handle.cdc_hdl, &line_coding));
    ESP_LOGI(TAG, "[CDC] Line Get: Rate: %"PRIu32", Stop bits: %"PRIu8", Parity: %"PRIu8", Databits: %"PRIu8"",
             line_coding.dwDTERate, line_coding.bCharFormat, line_coding.bParityType, line_coding.bDataBits);

    line_coding.dwDTERate = 9600;
    line_coding.bDataBits = 7;
    line_coding.bParityType = 1;
    line_coding.bCharFormat = 1;
    ESP_ERROR_CHECK(cdc_acm_host_line_coding_set(dev_info->handle.cdc_hdl, &line_coding));
    ESP_LOGI(TAG, "[CDC] Line Set: Rate: %"PRIu32", Stop bits: %"PRIu8", Parity: %"PRIu8", Databits: %"PRIu8"",
             line_coding.dwDTERate, line_coding.bCharFormat, line_coding.bParityType, line_coding.bDataBits);

    ESP_ERROR_CHECK(cdc_acm_host_line_coding_get(dev_info->handle.cdc_hdl, &line_coding));
    ESP_LOGI(TAG, "[CDC] Line Get: Rate: %"PRIu32", Stop bits: %"PRIu8", Parity: %"PRIu8", Databits: %"PRIu8"",
             line_coding.dwDTERate, line_coding.bCharFormat, line_coding.bParityType, line_coding.bDataBits);

    // Set control line state: DTR=1, RTS=0
    ESP_ERROR_CHECK(cdc_acm_host_set_control_line_state(dev_info->handle.cdc_hdl, true, false));
    ESP_LOGI(TAG, "[CDC] Control line state set: DTR=1, RTS=0");

    ESP_LOGI(TAG, "[CDC] Example finished successfully! Waiting for disconnection...");
}

/**
 * @brief Handle FTDI device
 *
 * Opens and operates an FTDI device based on usb_ftdi_example_main.c
 *
 * @param dev_info Device information structure
 */
static void handle_ftdi_device(device_info_t *dev_info)
{
    ESP_LOGI(TAG, "Opening FTDI device (VID=0x%04X, PID=0x%04X)", dev_info->vid, dev_info->pid);

    ftdi_sio_host_device_config_t dev_config = FTDI_SIO_HOST_DEVICE_CONFIG_DEFAULT();
    dev_config.event_cb = ftdi_handle_event;
    dev_config.data_cb = ftdi_handle_rx;
    dev_config.user_arg = dev_info;  // Pass dev_info for callback access

    esp_err_t err = ftdi_sio_host_open(dev_info->vid, dev_info->pid, 0, &dev_config, &dev_info->handle.ftdi_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open FTDI device: %s", esp_err_to_name(err));
        return;
    }

    dev_info->state = DEVICE_STATE_OPEN;
    ESP_LOGI(TAG, "[FTDI] Device opened successfully");

    vTaskDelay(pdMS_TO_TICKS(100));

    // Test sending data
    ESP_ERROR_CHECK(ftdi_sio_host_data_tx_blocking(dev_info->handle.ftdi_hdl,
                                                     (const uint8_t *)EXAMPLE_TX_STRING,
                                                     strlen(EXAMPLE_TX_STRING),
                                                     EXAMPLE_TX_TIMEOUT_MS));
    vTaskDelay(pdMS_TO_TICKS(100));

    // Test line settings: Set 115200 baud, 7 data bits, odd parity, 1 stop bit
    ESP_LOGI(TAG, "[FTDI] Setting up line configuration");

    ESP_ERROR_CHECK(ftdi_sio_host_set_baudrate(dev_info->handle.ftdi_hdl, 115200));
    ESP_LOGI(TAG, "[FTDI] Baudrate set to 115200");

    ESP_ERROR_CHECK(ftdi_sio_host_set_line_property(dev_info->handle.ftdi_hdl,
                                                      FTDI_DATA_BITS_7,
                                                      FTDI_STOP_BITS_1,
                                                      FTDI_PARITY_ODD));
    ESP_LOGI(TAG, "[FTDI] Line property set: 7 data bits, odd parity, 1 stop bit");

    // Test modem control: Set DTR=1, RTS=0
    ESP_ERROR_CHECK(ftdi_sio_host_set_modem_control(dev_info->handle.ftdi_hdl, true, false));
    ESP_LOGI(TAG, "[FTDI] Modem control set: DTR=1, RTS=0");

    // Get modem status
    ftdi_modem_status_t status;
    ESP_ERROR_CHECK(ftdi_sio_host_get_modem_status(dev_info->handle.ftdi_hdl, &status));
    ESP_LOGI(TAG, "[FTDI] Modem status: CTS=%d DSR=%d RI=%d CD=%d",
             status.cts, status.dsr, status.ri, status.rlsd);

    // Test modem control: Set DTR=0, RTS=0
    ESP_ERROR_CHECK(ftdi_sio_host_set_modem_control(dev_info->handle.ftdi_hdl, false, false));
    ESP_LOGI(TAG, "[FTDI] Modem control set: DTR=0, RTS=0");

    // Set latency timer to 16ms (default is typically 16ms)
    ESP_ERROR_CHECK(ftdi_sio_host_set_latency_timer(dev_info->handle.ftdi_hdl, 16));
    ESP_LOGI(TAG, "[FTDI] Latency timer set to 16ms");

    ESP_LOGI(TAG, "[FTDI] Example finished successfully! Waiting for disconnection...");
}

/**
 * @brief Main device handler dispatcher
 *
 * Dispatches device handling to the appropriate handler based on device type.
 *
 * @param dev_info Device information structure
 */
static void handle_device(device_info_t *dev_info)
{
    // Create disconnection semaphore
    dev_info->disconnected_sem = xSemaphoreCreateBinary();
    if (dev_info->disconnected_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create disconnection semaphore");
        return;
    }

    // Set current device pointer for bridge tasks
    current_device = dev_info;

    // Dispatch to appropriate handler
    if (dev_info->type == DEVICE_TYPE_CDC) {
        handle_cdc_device(dev_info);
    } else if (dev_info->type == DEVICE_TYPE_FTDI) {
        handle_ftdi_device(dev_info);
    } else {
        ESP_LOGE(TAG, "Unknown device type: %d", dev_info->type);
        current_device = NULL;
        vSemaphoreDelete(dev_info->disconnected_sem);
        return;
    }

    // Wait for disconnection
    ESP_LOGI(TAG, "Waiting for device disconnection...");
    xSemaphoreTake(dev_info->disconnected_sem, portMAX_DELAY);
    vSemaphoreDelete(dev_info->disconnected_sem);

    // Clear current device pointer
    current_device = NULL;

    ESP_LOGI(TAG, "Device disconnected, ready for next device");
}

// ============= MAIN APPLICATION =============

/**
 * @brief Main application
 *
 * Installs both CDC-ACM and FTDI drivers and automatically handles devices
 * based on their VID/PID. Also sets up WiFi and TCP server for network bridging.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "USB Serial to TCP Bridge with Network Provisioning");

    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize WiFi (required before provisioning check)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 3. Initialize TCP/IP and event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_event_group = xEventGroupCreate();

    // 4. Initialize provisioning manager
    ESP_LOGI(TAG, "Initializing provisioning manager...");
    ESP_ERROR_CHECK(init_provisioning_manager());

    

    // Register WiFi event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                          ESP_EVENT_ANY_ID,
                                                          &wifi_event_handler,
                                                          NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                          IP_EVENT_STA_GOT_IP,
                                                          &wifi_event_handler,
                                                          NULL, NULL));

    // 4. Create WiFi STA and AP interface and register event handlers
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    // 5. Check if already provisioned
    bool provisioned = is_provisioned();
    ESP_LOGI(TAG, "Provisioning status: %s", provisioned ? "DONE" : "NOT DONE");

    if (!provisioned) {
        // Not provisioned - start provisioning
        ESP_LOGI(TAG, "Starting provisioning...");
        ESP_LOGI(TAG, "Connect to SoftAP (SSID: PROV_xxxxxx) and access provisioning page");

        start_provisioning(&wifi_event_group, PROV_SUCCESS_BIT, PROV_FAIL_BIT);

        // Wait for provisioning to complete
        EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                                PROV_SUCCESS_BIT | PROV_FAIL_BIT,
                                                pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & PROV_FAIL_BIT) {
            ESP_LOGE(TAG, "Provisioning failed");
            return;
        }
        ESP_LOGI(TAG, "Provisioning successful");
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH)); // Auto-load from NVS
    ESP_ERROR_CHECK(esp_wifi_start());

    // 7. Wait for WiFi connection
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi");
    } else {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
        return;
    }

    // Initialize buffer pool
    ESP_LOGI(TAG, "Initializing buffer pool...");
    buffer_pool_init();

    // Create queues for data bridging (stores buffer pointers)
    ESP_LOGI(TAG, "Creating data queues...");
    usb_to_tcp_queue = xQueueCreate(8, sizeof(data_buffer_t*));
    tcp_to_usb_queue = xQueueCreate(8, sizeof(data_buffer_t*));
    device_queue = xQueueCreate(4, sizeof(device_info_t));

    if (usb_to_tcp_queue == NULL || tcp_to_usb_queue == NULL || device_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create queues");
        return;
    }

    // Create TCP server mutex
    tcp_server.tx_mutex = xSemaphoreCreateMutex();
    if (tcp_server.tx_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create TCP TX mutex");
        return;
    }

    // Initialize TCP server state
    tcp_server.client_sock = -1;
    tcp_server.connected = false;

    // Start TCP server task
    ESP_LOGI(TAG, "Starting TCP server on port %d...", CONFIG_TCP_SERVER_PORT);
    BaseType_t task_created = xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
    if (task_created != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create TCP server task");
        return;
    }

    // Start bridge tasks
    ESP_LOGI(TAG, "Starting bridge tasks...");
    task_created = xTaskCreate(usb_to_tcp_bridge_task, "usb_to_tcp", 4096, NULL, 6, NULL);
    if (task_created != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create USB→TCP bridge task");
        return;
    }

    task_created = xTaskCreate(tcp_to_usb_bridge_task, "tcp_to_usb", 4096, NULL, 6, NULL);
    if (task_created != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create TCP→USB bridge task");
        return;
    }

    // Install USB Host driver (shared by both drivers)
    ESP_LOGI(TAG, "Installing USB Host");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    // Create USB library handling task
    task_created = xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, EXAMPLE_USB_HOST_PRIORITY, NULL);
    if (task_created != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create USB library task");
        return;
    }

    // Install CDC-ACM driver with new_dev_cb
    ESP_LOGI(TAG, "Installing CDC-ACM driver");
    cdc_acm_host_driver_config_t cdc_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 5,
        .xCoreID = 1,
        .new_dev_cb = cdc_new_device_callback
    };
    ESP_ERROR_CHECK(cdc_acm_host_install(&cdc_config));

    // Install FTDI driver with new_dev_cb
    ESP_LOGI(TAG, "Installing FTDI driver");
    ftdi_sio_host_driver_config_t ftdi_config = FTDI_SIO_HOST_DRIVER_CONFIG_DEFAULT();
    ftdi_config.new_dev_cb = ftdi_new_device_callback;
    ftdi_config.user_arg = NULL;
    ESP_ERROR_CHECK(ftdi_sio_host_install(&ftdi_config));

    ESP_LOGI(TAG, "All systems initialized. Waiting for USB devices...");

    // Main device handling loop
    while (true) {
        device_info_t dev_info;

        // Wait for a device to be added to the queue
        if (xQueueReceive(device_queue, &dev_info, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Received device from queue: Type=%s VID=0x%04X PID=0x%04X",
                     dev_info.type == DEVICE_TYPE_CDC ? "CDC" :
                     dev_info.type == DEVICE_TYPE_FTDI ? "FTDI" : "UNKNOWN",
                     dev_info.vid, dev_info.pid);

            // Handle the device
            handle_device(&dev_info);
        }
    }
}
