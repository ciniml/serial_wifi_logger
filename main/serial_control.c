/*
 * SPDX-FileCopyrightText: 2024 Kenta IDA
 * SPDX-License-Identifier: Apache-2.0
 *
 * Serial Control Abstraction Layer Implementation
 */

#include "serial_control.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "usb/cdc_acm_host.h"
#include "usb/ftdi_sio_host.h"
#include "usb/ftdi_sio_host_ops.h"
#include "usb/ftdi_host_types.h"

static const char *TAG = "serial_ctrl";

// ============================================================================
// External references to main.c globals
// ============================================================================

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

// These are defined in main.c
extern device_info_t *current_device;
extern SemaphoreHandle_t device_mutex;

// ============================================================================
// Internal state tracking
// ============================================================================

static bool s_current_dtr = false;
static bool s_current_rts = false;
static uint32_t s_current_baudrate = 115200;
static uint8_t s_current_data_bits = 8;
static uint8_t s_current_parity = 0;
static uint8_t s_current_stop_bits = 0;

// ============================================================================
// Retry configuration
// ============================================================================

#ifndef CONFIG_SERIAL_CTRL_RETRY_INTERVAL_MS
#define CONFIG_SERIAL_CTRL_RETRY_INTERVAL_MS 10
#endif

#ifndef CONFIG_SERIAL_CTRL_RETRY_COUNT
#define CONFIG_SERIAL_CTRL_RETRY_COUNT 10
#endif

// ============================================================================
// Helper macros
// ============================================================================

#define ACQUIRE_DEVICE_MUTEX() \
    do { \
        if (xSemaphoreTake(device_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) { \
            ESP_LOGE(TAG, "Failed to acquire device mutex"); \
            return ESP_ERR_TIMEOUT; \
        } \
    } while(0)

#define RELEASE_DEVICE_MUTEX() xSemaphoreGive(device_mutex)

#define CHECK_DEVICE_CONNECTED() \
    do { \
        if (current_device == NULL || current_device->state != DEVICE_STATE_OPEN) { \
            RELEASE_DEVICE_MUTEX(); \
            return ESP_ERR_INVALID_STATE; \
        } \
    } while(0)

// ============================================================================
// Implementation
// ============================================================================

bool serial_control_is_connected(void)
{
    if (xSemaphoreTake(device_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    bool connected = (current_device != NULL && current_device->state == DEVICE_STATE_OPEN);
    xSemaphoreGive(device_mutex);
    return connected;
}

esp_err_t serial_control_set_line_coding(const serial_line_coding_t *coding)
{
    if (coding == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ACQUIRE_DEVICE_MUTEX();
    CHECK_DEVICE_CONNECTED();

    esp_err_t ret = ESP_OK;
    int retry_count = 0;

    if (current_device->type == DEVICE_TYPE_CDC) {
        cdc_acm_line_coding_t cdc_coding = {
            .dwDTERate = coding->baudrate,
            .bDataBits = coding->data_bits,
            .bParityType = coding->parity,
            .bCharFormat = coding->stop_bits
        };

        // Retry loop for CDC
        do {
            ret = cdc_acm_host_line_coding_set(current_device->handle.cdc_hdl, &cdc_coding);
            if (ret == ESP_ERR_NOT_FINISHED) {
                retry_count++;
                RELEASE_DEVICE_MUTEX();
                vTaskDelay(pdMS_TO_TICKS(CONFIG_SERIAL_CTRL_RETRY_INTERVAL_MS));
                ACQUIRE_DEVICE_MUTEX();
                CHECK_DEVICE_CONNECTED();
            }
        } while (ret == ESP_ERR_NOT_FINISHED && retry_count < CONFIG_SERIAL_CTRL_RETRY_COUNT);

    } else if (current_device->type == DEVICE_TYPE_FTDI) {
        // Set baudrate with retry
        do {
            ret = ftdi_sio_host_set_baudrate(current_device->handle.ftdi_hdl, coding->baudrate);
            if (ret == ESP_ERR_NOT_FINISHED) {
                retry_count++;
                RELEASE_DEVICE_MUTEX();
                vTaskDelay(pdMS_TO_TICKS(CONFIG_SERIAL_CTRL_RETRY_INTERVAL_MS));
                ACQUIRE_DEVICE_MUTEX();
                CHECK_DEVICE_CONNECTED();
            }
        } while (ret == ESP_ERR_NOT_FINISHED && retry_count < CONFIG_SERIAL_CTRL_RETRY_COUNT);

        if (ret == ESP_OK) {
            // Set line property
            ftdi_data_bits_t data_bits;
            switch (coding->data_bits) {
                case 7: data_bits = FTDI_DATA_BITS_7; break;
                case 8:
                default: data_bits = FTDI_DATA_BITS_8; break;
            }

            ftdi_stop_bits_t stop_bits;
            switch (coding->stop_bits) {
                case 1: stop_bits = FTDI_STOP_BITS_15; break;
                case 2: stop_bits = FTDI_STOP_BITS_2; break;
                case 0:
                default: stop_bits = FTDI_STOP_BITS_1; break;
            }

            ftdi_parity_t parity;
            switch (coding->parity) {
                case 1: parity = FTDI_PARITY_ODD; break;
                case 2: parity = FTDI_PARITY_EVEN; break;
                case 3: parity = FTDI_PARITY_MARK; break;
                case 4: parity = FTDI_PARITY_SPACE; break;
                case 0:
                default: parity = FTDI_PARITY_NONE; break;
            }

            // Retry loop for line property
            retry_count = 0;
            do {
                ret = ftdi_sio_host_set_line_property(current_device->handle.ftdi_hdl,
                                                       data_bits, stop_bits, parity);
                if (ret == ESP_ERR_NOT_FINISHED) {
                    retry_count++;
                    RELEASE_DEVICE_MUTEX();
                    vTaskDelay(pdMS_TO_TICKS(CONFIG_SERIAL_CTRL_RETRY_INTERVAL_MS));
                    ACQUIRE_DEVICE_MUTEX();
                    CHECK_DEVICE_CONNECTED();
                }
            } while (ret == ESP_ERR_NOT_FINISHED && retry_count < CONFIG_SERIAL_CTRL_RETRY_COUNT);
        }
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }

    if (ret == ESP_OK) {
        s_current_baudrate = coding->baudrate;
        s_current_data_bits = coding->data_bits;
        s_current_parity = coding->parity;
        s_current_stop_bits = coding->stop_bits;
        ESP_LOGD(TAG, "Line coding set: %lu bps, %d data, %d parity, %d stop",
                 (unsigned long)coding->baudrate, coding->data_bits,
                 coding->parity, coding->stop_bits);
    }

    RELEASE_DEVICE_MUTEX();
    return ret;
}

esp_err_t serial_control_get_line_coding(serial_line_coding_t *coding)
{
    if (coding == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ACQUIRE_DEVICE_MUTEX();
    CHECK_DEVICE_CONNECTED();

    esp_err_t ret = ESP_OK;

    if (current_device->type == DEVICE_TYPE_CDC) {
        cdc_acm_line_coding_t cdc_coding;
        ret = cdc_acm_host_line_coding_get(current_device->handle.cdc_hdl, &cdc_coding);
        if (ret == ESP_OK) {
            coding->baudrate = cdc_coding.dwDTERate;
            coding->data_bits = cdc_coding.bDataBits;
            coding->parity = cdc_coding.bParityType;
            coding->stop_bits = cdc_coding.bCharFormat;
        }
    } else if (current_device->type == DEVICE_TYPE_FTDI) {
        // FTDI doesn't support reading back line coding, use cached values
        coding->baudrate = s_current_baudrate;
        coding->data_bits = s_current_data_bits;
        coding->parity = s_current_parity;
        coding->stop_bits = s_current_stop_bits;
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }

    RELEASE_DEVICE_MUTEX();
    return ret;
}

esp_err_t serial_control_set_baudrate(uint32_t baudrate)
{
    ACQUIRE_DEVICE_MUTEX();
    CHECK_DEVICE_CONNECTED();

    esp_err_t ret = ESP_OK;

    if (current_device->type == DEVICE_TYPE_CDC) {
        cdc_acm_line_coding_t cdc_coding;
        ret = cdc_acm_host_line_coding_get(current_device->handle.cdc_hdl, &cdc_coding);
        if (ret == ESP_OK) {
            cdc_coding.dwDTERate = baudrate;
            ret = cdc_acm_host_line_coding_set(current_device->handle.cdc_hdl, &cdc_coding);
        }
    } else if (current_device->type == DEVICE_TYPE_FTDI) {
        ret = ftdi_sio_host_set_baudrate(current_device->handle.ftdi_hdl, baudrate);
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }

    if (ret == ESP_OK) {
        s_current_baudrate = baudrate;
        ESP_LOGD(TAG, "Baudrate set: %lu", (unsigned long)baudrate);
    }

    RELEASE_DEVICE_MUTEX();
    return ret;
}

esp_err_t serial_control_get_baudrate(uint32_t *baudrate)
{
    if (baudrate == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ACQUIRE_DEVICE_MUTEX();
    CHECK_DEVICE_CONNECTED();

    esp_err_t ret = ESP_OK;

    if (current_device->type == DEVICE_TYPE_CDC) {
        cdc_acm_line_coding_t cdc_coding;
        ret = cdc_acm_host_line_coding_get(current_device->handle.cdc_hdl, &cdc_coding);
        if (ret == ESP_OK) {
            *baudrate = cdc_coding.dwDTERate;
        }
    } else if (current_device->type == DEVICE_TYPE_FTDI) {
        *baudrate = s_current_baudrate;
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }

    RELEASE_DEVICE_MUTEX();
    return ret;
}

esp_err_t serial_control_set_dtr(bool state)
{
    ACQUIRE_DEVICE_MUTEX();
    CHECK_DEVICE_CONNECTED();

    esp_err_t ret = ESP_OK;
    int retry_count = 0;
    s_current_dtr = state;

    if (current_device->type == DEVICE_TYPE_CDC) {
        do {
            ret = cdc_acm_host_set_control_line_state(current_device->handle.cdc_hdl,
                                                       s_current_dtr, s_current_rts);
            if (ret == ESP_ERR_NOT_FINISHED) {
                retry_count++;
                RELEASE_DEVICE_MUTEX();
                vTaskDelay(pdMS_TO_TICKS(CONFIG_SERIAL_CTRL_RETRY_INTERVAL_MS));
                ACQUIRE_DEVICE_MUTEX();
                CHECK_DEVICE_CONNECTED();
            }
        } while (ret == ESP_ERR_NOT_FINISHED && retry_count < CONFIG_SERIAL_CTRL_RETRY_COUNT);
    } else if (current_device->type == DEVICE_TYPE_FTDI) {
        do {
            ret = ftdi_sio_host_set_modem_control(current_device->handle.ftdi_hdl,
                                                   s_current_dtr, s_current_rts);
            if (ret == ESP_ERR_NOT_FINISHED) {
                retry_count++;
                RELEASE_DEVICE_MUTEX();
                vTaskDelay(pdMS_TO_TICKS(CONFIG_SERIAL_CTRL_RETRY_INTERVAL_MS));
                ACQUIRE_DEVICE_MUTEX();
                CHECK_DEVICE_CONNECTED();
            }
        } while (ret == ESP_ERR_NOT_FINISHED && retry_count < CONFIG_SERIAL_CTRL_RETRY_COUNT);
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "DTR set: %d", state);
    }

    RELEASE_DEVICE_MUTEX();
    return ret;
}

esp_err_t serial_control_get_dtr(bool *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *state = s_current_dtr;
    return ESP_OK;
}

esp_err_t serial_control_set_rts(bool state)
{
    ACQUIRE_DEVICE_MUTEX();
    CHECK_DEVICE_CONNECTED();

    esp_err_t ret = ESP_OK;
    int retry_count = 0;
    s_current_rts = state;

    if (current_device->type == DEVICE_TYPE_CDC) {
        do {
            ret = cdc_acm_host_set_control_line_state(current_device->handle.cdc_hdl,
                                                       s_current_dtr, s_current_rts);
            if (ret == ESP_ERR_NOT_FINISHED) {
                retry_count++;
                RELEASE_DEVICE_MUTEX();
                vTaskDelay(pdMS_TO_TICKS(CONFIG_SERIAL_CTRL_RETRY_INTERVAL_MS));
                ACQUIRE_DEVICE_MUTEX();
                CHECK_DEVICE_CONNECTED();
            }
        } while (ret == ESP_ERR_NOT_FINISHED && retry_count < CONFIG_SERIAL_CTRL_RETRY_COUNT);
    } else if (current_device->type == DEVICE_TYPE_FTDI) {
        do {
            ret = ftdi_sio_host_set_modem_control(current_device->handle.ftdi_hdl,
                                                   s_current_dtr, s_current_rts);
            if (ret == ESP_ERR_NOT_FINISHED) {
                retry_count++;
                RELEASE_DEVICE_MUTEX();
                vTaskDelay(pdMS_TO_TICKS(CONFIG_SERIAL_CTRL_RETRY_INTERVAL_MS));
                ACQUIRE_DEVICE_MUTEX();
                CHECK_DEVICE_CONNECTED();
            }
        } while (ret == ESP_ERR_NOT_FINISHED && retry_count < CONFIG_SERIAL_CTRL_RETRY_COUNT);
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "RTS set: %d", state);
    }

    RELEASE_DEVICE_MUTEX();
    return ret;
}

esp_err_t serial_control_get_rts(bool *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *state = s_current_rts;
    return ESP_OK;
}

esp_err_t serial_control_set_modem_control(bool dtr, bool rts)
{
    ACQUIRE_DEVICE_MUTEX();
    CHECK_DEVICE_CONNECTED();

    esp_err_t ret = ESP_OK;
    int retry_count = 0;
    s_current_dtr = dtr;
    s_current_rts = rts;

    if (current_device->type == DEVICE_TYPE_CDC) {
        do {
            ret = cdc_acm_host_set_control_line_state(current_device->handle.cdc_hdl, dtr, rts);
            if (ret == ESP_ERR_NOT_FINISHED) {
                retry_count++;
                RELEASE_DEVICE_MUTEX();
                vTaskDelay(pdMS_TO_TICKS(CONFIG_SERIAL_CTRL_RETRY_INTERVAL_MS));
                ACQUIRE_DEVICE_MUTEX();
                CHECK_DEVICE_CONNECTED();
            }
        } while (ret == ESP_ERR_NOT_FINISHED && retry_count < CONFIG_SERIAL_CTRL_RETRY_COUNT);
    } else if (current_device->type == DEVICE_TYPE_FTDI) {
        do {
            ret = ftdi_sio_host_set_modem_control(current_device->handle.ftdi_hdl, dtr, rts);
            if (ret == ESP_ERR_NOT_FINISHED) {
                retry_count++;
                RELEASE_DEVICE_MUTEX();
                vTaskDelay(pdMS_TO_TICKS(CONFIG_SERIAL_CTRL_RETRY_INTERVAL_MS));
                ACQUIRE_DEVICE_MUTEX();
                CHECK_DEVICE_CONNECTED();
            }
        } while (ret == ESP_ERR_NOT_FINISHED && retry_count < CONFIG_SERIAL_CTRL_RETRY_COUNT);
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Modem control set: DTR=%d, RTS=%d", dtr, rts);
    }

    RELEASE_DEVICE_MUTEX();
    return ret;
}

esp_err_t serial_control_get_modem_status(serial_modem_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ACQUIRE_DEVICE_MUTEX();
    CHECK_DEVICE_CONNECTED();

    esp_err_t ret = ESP_OK;
    memset(status, 0, sizeof(serial_modem_status_t));

    if (current_device->type == DEVICE_TYPE_FTDI) {
        ftdi_modem_status_t ftdi_status;
        ret = ftdi_sio_host_get_modem_status(current_device->handle.ftdi_hdl, &ftdi_status);
        if (ret == ESP_OK) {
            status->cts = ftdi_status.cts;
            status->dsr = ftdi_status.dsr;
            status->ri = ftdi_status.ri;
            status->cd = ftdi_status.rlsd;
        }
    } else if (current_device->type == DEVICE_TYPE_CDC) {
        // CDC-ACM doesn't support reading modem status
        // Return default values (all off)
        ret = ESP_ERR_NOT_SUPPORTED;
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }

    RELEASE_DEVICE_MUTEX();
    return ret;
}

esp_err_t serial_control_set_break(bool on)
{
    ACQUIRE_DEVICE_MUTEX();
    CHECK_DEVICE_CONNECTED();

    esp_err_t ret = ESP_OK;

    if (current_device->type == DEVICE_TYPE_CDC) {
        // CDC-ACM break control via send_break
        if (on) {
            ret = cdc_acm_host_send_break(current_device->handle.cdc_hdl, 100);
        }
        // Note: CDC break is time-limited, can't be turned off explicitly
    } else if (current_device->type == DEVICE_TYPE_FTDI) {
        // FTDI break control
        // Note: FTDI driver may not expose break control directly
        // This is a placeholder - actual implementation depends on driver support
        ESP_LOGW(TAG, "Break control not fully supported on FTDI");
        ret = ESP_ERR_NOT_SUPPORTED;
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }

    RELEASE_DEVICE_MUTEX();
    return ret;
}

esp_err_t serial_control_transmit(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ACQUIRE_DEVICE_MUTEX();
    CHECK_DEVICE_CONNECTED();

    esp_err_t ret = ESP_OK;
    int retry_count = 0;

    if (current_device->type == DEVICE_TYPE_CDC) {
        do {
            ret = cdc_acm_host_data_tx_blocking(current_device->handle.cdc_hdl,
                                                data, len, timeout_ms);
            if (ret == ESP_ERR_NOT_FINISHED) {
                retry_count++;
                RELEASE_DEVICE_MUTEX();
                vTaskDelay(pdMS_TO_TICKS(CONFIG_SERIAL_CTRL_RETRY_INTERVAL_MS));
                ACQUIRE_DEVICE_MUTEX();
                CHECK_DEVICE_CONNECTED();
            }
        } while (ret == ESP_ERR_NOT_FINISHED && retry_count < CONFIG_SERIAL_CTRL_RETRY_COUNT);
    } else if (current_device->type == DEVICE_TYPE_FTDI) {
        do {
            ret = ftdi_sio_host_data_tx_blocking(current_device->handle.ftdi_hdl,
                                                  data, len, timeout_ms);
            if (ret == ESP_ERR_NOT_FINISHED) {
                retry_count++;
                RELEASE_DEVICE_MUTEX();
                vTaskDelay(pdMS_TO_TICKS(CONFIG_SERIAL_CTRL_RETRY_INTERVAL_MS));
                ACQUIRE_DEVICE_MUTEX();
                CHECK_DEVICE_CONNECTED();
            }
        } while (ret == ESP_ERR_NOT_FINISHED && retry_count < CONFIG_SERIAL_CTRL_RETRY_COUNT);
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }

    RELEASE_DEVICE_MUTEX();
    return ret;
}
