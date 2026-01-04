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

// ============= GLOBAL VARIABLES =============

static QueueHandle_t device_queue;

// ============= FORWARD DECLARATIONS =============

static void usb_lib_task(void *arg);
static void cdc_new_device_callback(usb_device_handle_t usb_dev);
static void ftdi_new_device_callback(uint16_t vid, uint16_t pid, void *user_arg);
static bool cdc_handle_rx(const uint8_t *data, size_t data_len, void *arg);
static void ftdi_handle_rx(const uint8_t *data, size_t data_len, void *arg);
static void cdc_handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx);
static void ftdi_handle_event(ftdi_sio_host_dev_event_t event, void *user_ctx);
static void handle_cdc_device(device_info_t *dev_info);
static void handle_ftdi_device(device_info_t *dev_info);
static void handle_device(device_info_t *dev_info);

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
static bool cdc_handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
    ESP_LOGI(TAG, "[CDC] Data received (%d bytes)", data_len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_len, ESP_LOG_INFO);
    return true;  // Data processed
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
    ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_len, ESP_LOG_INFO);
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
        .in_buffer_size = 512,
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

    // Dispatch to appropriate handler
    if (dev_info->type == DEVICE_TYPE_CDC) {
        handle_cdc_device(dev_info);
    } else if (dev_info->type == DEVICE_TYPE_FTDI) {
        handle_ftdi_device(dev_info);
    } else {
        ESP_LOGE(TAG, "Unknown device type: %d", dev_info->type);
        vSemaphoreDelete(dev_info->disconnected_sem);
        return;
    }

    // Wait for disconnection
    ESP_LOGI(TAG, "Waiting for device disconnection...");
    xSemaphoreTake(dev_info->disconnected_sem, portMAX_DELAY);
    vSemaphoreDelete(dev_info->disconnected_sem);

    ESP_LOGI(TAG, "Device disconnected, ready for next device");
}

// ============= MAIN APPLICATION =============

/**
 * @brief Main application
 *
 * Installs both CDC-ACM and FTDI drivers and automatically handles devices
 * based on their VID/PID.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "USB Serial Auto-Detection Example");
    ESP_LOGI(TAG, "Installing both CDC-ACM and FTDI drivers...");

    // Create device queue
    device_queue = xQueueCreate(4, sizeof(device_info_t));
    if (device_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create device queue");
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
    BaseType_t task_created = xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, EXAMPLE_USB_HOST_PRIORITY, NULL);
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

    ESP_LOGI(TAG, "Both drivers installed. Waiting for USB devices...");

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
