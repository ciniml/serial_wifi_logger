/*
 * SPDX-FileCopyrightText: 2024 Kenta IDA
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFC2217 Server Implementation
 */

#include "rfc2217_server.h"
#include "rfc2217_protocol.h"
#include "serial_control.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"

#include "version.h"

static const char *TAG = "rfc2217";

// ============================================================================
// Configuration
// ============================================================================

#ifndef CONFIG_RFC2217_PORT
#define CONFIG_RFC2217_PORT 2217
#endif

#ifndef CONFIG_RFC2217_MODEM_POLL_INTERVAL_MS
#define CONFIG_RFC2217_MODEM_POLL_INTERVAL_MS 100
#endif

#define RFC2217_RX_BUFFER_SIZE      256
#define RFC2217_TX_BUFFER_SIZE      1024
#define RFC2217_SERVER_TASK_STACK   4096
#define RFC2217_MODEM_TASK_STACK    2048

// ============================================================================
// Server state
// ============================================================================

typedef struct {
    int listen_sock;
    int client_sock;
    bool connected;
    bool running;
    rfc2217_session_t session;
    TaskHandle_t server_task;
    TaskHandle_t modem_poll_task;
    SemaphoreHandle_t tx_mutex;
    QueueHandle_t tx_queue;
} rfc2217_server_t;

static rfc2217_server_t s_server = {
    .listen_sock = -1,
    .client_sock = -1,
    .connected = false,
    .running = false,
};

// ============================================================================
// Forward declarations
// ============================================================================

static void rfc2217_server_task(void *pvParameters);
static void rfc2217_modem_poll_task(void *pvParameters);
static void handle_client_connection(int sock);
static esp_err_t send_negotiation(int sock);
static esp_err_t send_response(int sock, rfc2217_session_t *session);
static esp_err_t apply_serial_settings(rfc2217_session_t *session);

// ============================================================================
// Public API
// ============================================================================

esp_err_t rfc2217_server_init(void)
{
    if (s_server.running) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_server.tx_mutex = xSemaphoreCreateMutex();
    if (s_server.tx_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create TX mutex");
        return ESP_ERR_NO_MEM;
    }

    s_server.running = true;
    s_server.client_sock = -1;
    s_server.connected = false;

    BaseType_t ret = xTaskCreate(rfc2217_server_task, "rfc2217_srv",
                                  RFC2217_SERVER_TASK_STACK, NULL, 5,
                                  &s_server.server_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create server task");
        vSemaphoreDelete(s_server.tx_mutex);
        s_server.running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "RFC2217 server initialized (port %d)", CONFIG_RFC2217_PORT);
    return ESP_OK;
}

esp_err_t rfc2217_server_stop(void)
{
    if (!s_server.running) {
        return ESP_OK;
    }

    s_server.running = false;

    // Close sockets to unblock tasks
    if (s_server.client_sock >= 0) {
        shutdown(s_server.client_sock, SHUT_RDWR);
        close(s_server.client_sock);
        s_server.client_sock = -1;
    }
    if (s_server.listen_sock >= 0) {
        close(s_server.listen_sock);
        s_server.listen_sock = -1;
    }

    // Wait for tasks to finish
    vTaskDelay(pdMS_TO_TICKS(100));

    if (s_server.tx_mutex != NULL) {
        vSemaphoreDelete(s_server.tx_mutex);
        s_server.tx_mutex = NULL;
    }

    ESP_LOGI(TAG, "RFC2217 server stopped");
    return ESP_OK;
}

bool rfc2217_server_is_connected(void)
{
    return s_server.connected;
}

esp_err_t rfc2217_server_send_data(const uint8_t *data, size_t len)
{
    if (!s_server.connected || s_server.client_sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Escape data (IAC -> IAC IAC)
    uint8_t escaped[RFC2217_TX_BUFFER_SIZE];
    size_t escaped_len = rfc2217_escape_data(data, len, escaped, sizeof(escaped));

    if (xSemaphoreTake(s_server.tx_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int sent = 0;
    int remaining = escaped_len;
    while (remaining > 0 && s_server.connected) {
        int ret = send(s_server.client_sock, escaped + sent, remaining, 0);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // SO_SNDTIMEO fired: TCP window closed longer than timeout.
                // Drop this chunk to avoid blocking further; the USB-level backpressure
                // (blocking USB callbacks) should prevent data accumulation.
                ESP_LOGW(TAG, "TCP send timeout, dropped %d bytes", remaining);
                break;
            }
            ESP_LOGE(TAG, "Send failed: errno %d", errno);
            s_server.connected = false;
            xSemaphoreGive(s_server.tx_mutex);
            return ESP_FAIL;
        }
        sent += ret;
        remaining -= ret;
    }

    xSemaphoreGive(s_server.tx_mutex);
    return ESP_OK;
}

esp_err_t rfc2217_server_notify_modemstate(bool cts, bool dsr, bool ri, bool cd)
{
    if (!s_server.connected || s_server.client_sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t state = 0;
    if (cts) state |= RFC2217_MODEMSTATE_CTS;
    if (dsr) state |= RFC2217_MODEMSTATE_DSR;
    if (ri)  state |= RFC2217_MODEMSTATE_RI;
    if (cd)  state |= RFC2217_MODEMSTATE_CD;

    // Check mask
    if ((state & s_server.session.modemstate_mask) == 0 &&
        (s_server.session.last_modemstate & s_server.session.modemstate_mask) == 0) {
        return ESP_OK;  // No reportable change
    }

    // Compute delta bits
    uint8_t delta = (state ^ s_server.session.last_modemstate) & 0x0F;
    state |= delta;

    uint8_t msg[16];
    size_t msg_len = rfc2217_build_modemstate(state, msg);

    if (xSemaphoreTake(s_server.tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int ret = send(s_server.client_sock, msg, msg_len, 0);
    xSemaphoreGive(s_server.tx_mutex);

    if (ret < 0) {
        return ESP_FAIL;
    }

    s_server.session.last_modemstate = state & 0xF0;  // Store only steady-state bits
    return ESP_OK;
}

esp_err_t rfc2217_server_notify_linestate(uint8_t state)
{
    if (!s_server.connected || s_server.client_sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    // Check mask
    if ((state & s_server.session.linestate_mask) == 0) {
        return ESP_OK;
    }

    uint8_t msg[16];
    size_t msg_len = rfc2217_build_linestate(state, msg);

    if (xSemaphoreTake(s_server.tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int ret = send(s_server.client_sock, msg, msg_len, 0);
    xSemaphoreGive(s_server.tx_mutex);

    return (ret >= 0) ? ESP_OK : ESP_FAIL;
}

// ============================================================================
// Server task
// ============================================================================

static void rfc2217_server_task(void *pvParameters)
{
    struct sockaddr_in dest_addr = {
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_RFC2217_PORT)
    };

    // Create listening socket
    s_server.listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_server.listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        s_server.running = false;
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_server.listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int err = bind(s_server.listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(s_server.listen_sock);
        s_server.listen_sock = -1;
        s_server.running = false;
        vTaskDelete(NULL);
        return;
    }

    err = listen(s_server.listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Socket listen failed: errno %d", errno);
        close(s_server.listen_sock);
        s_server.listen_sock = -1;
        s_server.running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "RFC2217 server listening on port %d", CONFIG_RFC2217_PORT);

    while (s_server.running) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);

        int sock = accept(s_server.listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            if (s_server.running) {
                ESP_LOGE(TAG, "Accept failed: errno %d", errno);
            }
            continue;
        }

        // Close existing connection if any
        if (s_server.connected && s_server.client_sock >= 0) {
            ESP_LOGI(TAG, "New client connecting, closing existing connection");
            shutdown(s_server.client_sock, SHUT_RDWR);
            close(s_server.client_sock);

            // Stop modem poll task
            if (s_server.modem_poll_task != NULL) {
                vTaskDelete(s_server.modem_poll_task);
                s_server.modem_poll_task = NULL;
            }
        }

        char addr_str[16];
        inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "RFC2217 client connected from %s", addr_str);

        s_server.client_sock = sock;
        s_server.connected = true;

        // Initialize session
        char signature[64];
        snprintf(signature, sizeof(signature), "ESP32-S3 Serial WiFi Logger %s", get_version_string());
        rfc2217_session_init(&s_server.session, signature);

        // Start modem poll task
        xTaskCreate(rfc2217_modem_poll_task, "rfc2217_modem",
                    RFC2217_MODEM_TASK_STACK, NULL, 4,
                    &s_server.modem_poll_task);

        // Handle client connection
        handle_client_connection(sock);

        // Cleanup
        ESP_LOGI(TAG, "RFC2217 client disconnected");
        s_server.connected = false;

        if (s_server.modem_poll_task != NULL) {
            vTaskDelete(s_server.modem_poll_task);
            s_server.modem_poll_task = NULL;
        }

        close(sock);
        s_server.client_sock = -1;
    }

    if (s_server.listen_sock >= 0) {
        close(s_server.listen_sock);
        s_server.listen_sock = -1;
    }

    vTaskDelete(NULL);
}

// ============================================================================
// Client connection handler
// ============================================================================

static void handle_client_connection(int sock)
{
    uint8_t rx_buffer[RFC2217_RX_BUFFER_SIZE];
    uint8_t data_buffer[RFC2217_RX_BUFFER_SIZE];
    uint8_t tx_batch[RFC2217_RX_BUFFER_SIZE];
    size_t tx_batch_len = 0;

    // Send initial negotiation (WILL COM-PORT-OPTION)
    if (send_negotiation(sock) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send initial negotiation");
        return;
    }

    // Set socket timeouts
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct timeval tv_snd = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv_snd, sizeof(tv_snd));

    while (s_server.connected && s_server.running) {
        int len = recv(sock, rx_buffer, sizeof(rx_buffer), 0);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout - continue
                continue;
            }
            ESP_LOGE(TAG, "Recv error: errno %d", errno);
            break;
        } else if (len == 0) {
            // Connection closed
            break;
        }

        // Process received data
        tx_batch_len = 0;
        for (int i = 0; i < len; i++) {
            size_t out_len = 0;
            rfc2217_result_t result = rfc2217_parse_byte(&s_server.session, rx_buffer[i],
                                                          data_buffer, &out_len);

            switch (result) {
                case RFC2217_RESULT_DATA:
                    // Accumulate data bytes for batched transmission
                    if (out_len > 0) {
                        tx_batch[tx_batch_len++] = data_buffer[0];
                    }
                    break;

                case RFC2217_RESULT_COMMAND:
                    // Flush batched data before applying settings/sending response
                    if (tx_batch_len > 0) {
                        esp_err_t err = serial_control_transmit(tx_batch, tx_batch_len, 1000);
                        if (err != ESP_OK) {
                            ESP_LOGW(TAG, "Serial transmit failed: %s", esp_err_to_name(err));
                        }
                        tx_batch_len = 0;
                    }
                    if (s_server.session.settings_changed) {
                        apply_serial_settings(&s_server.session);
                        s_server.session.settings_changed = false;
                    }
                    send_response(sock, &s_server.session);
                    break;

                case RFC2217_RESULT_CONTINUE:
                    // Continue parsing
                    break;

                case RFC2217_RESULT_ERROR:
                    ESP_LOGW(TAG, "Parse error");
                    break;
            }
        }

        // Flush any remaining batched data
        if (tx_batch_len > 0) {
            esp_err_t err = serial_control_transmit(tx_batch, tx_batch_len, 1000);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Serial transmit failed: %s", esp_err_to_name(err));
            }
        }
    }
}

// ============================================================================
// Negotiation and response
// ============================================================================

static esp_err_t send_negotiation(int sock)
{
    if (xSemaphoreTake(s_server.tx_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t msg[8];
    esp_err_t ret = ESP_OK;

    // Negotiate BINARY mode (RFC 856) - required for correct 8-bit data transfer
    msg[0] = TELNET_IAC; msg[1] = TELNET_WILL; msg[2] = TELNET_OPTION_BINARY;
    if (send(sock, msg, 3, 0) < 0) { ret = ESP_FAIL; goto done; }
    msg[0] = TELNET_IAC; msg[1] = TELNET_DO;   msg[2] = TELNET_OPTION_BINARY;
    if (send(sock, msg, 3, 0) < 0) { ret = ESP_FAIL; goto done; }

    // Negotiate Suppress Go Ahead (RFC 858)
    msg[0] = TELNET_IAC; msg[1] = TELNET_WILL; msg[2] = TELNET_OPTION_SGA;
    if (send(sock, msg, 3, 0) < 0) { ret = ESP_FAIL; goto done; }
    msg[0] = TELNET_IAC; msg[1] = TELNET_DO;   msg[2] = TELNET_OPTION_SGA;
    if (send(sock, msg, 3, 0) < 0) { ret = ESP_FAIL; goto done; }

    // Send WILL COM-PORT-OPTION
    {
        size_t len = rfc2217_build_will_com_port(msg);
        if (send(sock, msg, len, 0) < 0) { ret = ESP_FAIL; goto done; }

        // Send DO COM-PORT-OPTION (request client to enable)
        len = rfc2217_build_do_com_port(msg);
        if (send(sock, msg, len, 0) < 0) { ret = ESP_FAIL; goto done; }
    }

    ESP_LOGD(TAG, "Sent initial negotiation (BINARY, SGA, COM-PORT-OPTION)");

done:
    xSemaphoreGive(s_server.tx_mutex);
    return ret;
}

static esp_err_t send_response(int sock, rfc2217_session_t *session)
{
    // Protect all sends with tx_mutex to prevent races with rfc2217_server_send_data
    // (called from usb_to_tcp_bridge_task) and rfc2217_server_notify_modemstate
    // (called from rfc2217_modem_poll_task).
    if (xSemaphoreTake(s_server.tx_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        session->last_command = 0xFF;
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    uint8_t msg[128];
    size_t msg_len = 0;

    // Send pending Telnet option negotiation response (WILL/WONT/DO/DONT)
    if (session->need_option_response) {
        uint8_t opt_msg[3] = {TELNET_IAC, session->option_response_cmd, session->option_response_opt};
        if (send(sock, opt_msg, 3, 0) < 0) {
            ret = ESP_FAIL;
            goto done;
        }
        ESP_LOGD(TAG, "Sent option response: cmd=0x%02X opt=%d",
                 session->option_response_cmd, session->option_response_opt);
        session->need_option_response = false;
    }

    // Send response based on the last command processed
    switch (session->last_command) {
        case RFC2217_SIGNATURE:
            if (session->need_signature_response) {
                msg_len = rfc2217_build_signature(session->signature, msg);
                if (send(sock, msg, msg_len, 0) < 0) { ret = ESP_FAIL; goto done; }
                session->need_signature_response = false;
                ESP_LOGD(TAG, "Sent signature response");
            }
            break;

        case RFC2217_SET_BAUDRATE:
            msg_len = rfc2217_build_baudrate_response(session->baudrate, msg);
            if (send(sock, msg, msg_len, 0) < 0) { ret = ESP_FAIL; goto done; }
            ESP_LOGD(TAG, "Sent baudrate response: %lu", (unsigned long)session->baudrate);
            break;

        case RFC2217_SET_DATASIZE:
            msg_len = rfc2217_build_byte_response(RFC2217_RESP_SET_DATASIZE, session->datasize, msg);
            if (send(sock, msg, msg_len, 0) < 0) { ret = ESP_FAIL; goto done; }
            ESP_LOGD(TAG, "Sent datasize response: %d", session->datasize);
            break;

        case RFC2217_SET_PARITY:
            msg_len = rfc2217_build_byte_response(RFC2217_RESP_SET_PARITY, session->parity, msg);
            if (send(sock, msg, msg_len, 0) < 0) { ret = ESP_FAIL; goto done; }
            ESP_LOGD(TAG, "Sent parity response: %d", session->parity);
            break;

        case RFC2217_SET_STOPSIZE:
            msg_len = rfc2217_build_byte_response(RFC2217_RESP_SET_STOPSIZE, session->stopsize, msg);
            if (send(sock, msg, msg_len, 0) < 0) { ret = ESP_FAIL; goto done; }
            ESP_LOGD(TAG, "Sent stopsize response: %d", session->stopsize);
            break;

        case RFC2217_SET_CONTROL:
            {
                uint8_t response_value = 0;
                uint8_t control_val = session->last_control_value;

                if (control_val == RFC2217_CONTROL_FLOW_REQUEST) {
                    response_value = session->flowcontrol;
                } else if (control_val >= RFC2217_CONTROL_FLOW_NONE &&
                           control_val <= RFC2217_CONTROL_FLOW_HARDWARE) {
                    response_value = control_val;
                } else if (control_val == RFC2217_CONTROL_BREAK_REQUEST) {
                    response_value = session->break_state ? RFC2217_CONTROL_BREAK_ON : RFC2217_CONTROL_BREAK_OFF;
                } else if (control_val == RFC2217_CONTROL_BREAK_ON ||
                           control_val == RFC2217_CONTROL_BREAK_OFF) {
                    response_value = control_val;
                } else if (control_val == RFC2217_CONTROL_DTR_REQUEST) {
                    response_value = session->dtr ? RFC2217_CONTROL_DTR_ON : RFC2217_CONTROL_DTR_OFF;
                } else if (control_val == RFC2217_CONTROL_DTR_ON ||
                           control_val == RFC2217_CONTROL_DTR_OFF) {
                    response_value = control_val;
                } else if (control_val == RFC2217_CONTROL_RTS_REQUEST) {
                    response_value = session->rts ? RFC2217_CONTROL_RTS_ON : RFC2217_CONTROL_RTS_OFF;
                } else if (control_val == RFC2217_CONTROL_RTS_ON ||
                           control_val == RFC2217_CONTROL_RTS_OFF) {
                    response_value = control_val;
                } else {
                    response_value = control_val;
                }

                msg_len = rfc2217_build_byte_response(RFC2217_RESP_SET_CONTROL, response_value, msg);
                if (send(sock, msg, msg_len, 0) < 0) { ret = ESP_FAIL; goto done; }
                ESP_LOGD(TAG, "Sent control response: %d", response_value);
            }
            break;

        case RFC2217_SET_LINESTATE_MASK:
            msg_len = rfc2217_build_byte_response(RFC2217_RESP_SET_LINESTATE_MASK, session->linestate_mask, msg);
            if (send(sock, msg, msg_len, 0) < 0) { ret = ESP_FAIL; goto done; }
            ESP_LOGD(TAG, "Sent linestate mask response: 0x%02X", session->linestate_mask);
            break;

        case RFC2217_SET_MODEMSTATE_MASK:
            msg_len = rfc2217_build_byte_response(RFC2217_RESP_SET_MODEMSTATE_MASK, session->modemstate_mask, msg);
            if (send(sock, msg, msg_len, 0) < 0) { ret = ESP_FAIL; goto done; }
            ESP_LOGD(TAG, "Sent modemstate mask response: 0x%02X", session->modemstate_mask);
            break;

        case RFC2217_PURGE_DATA:
            msg_len = rfc2217_build_byte_response(RFC2217_RESP_PURGE_DATA, session->last_purge_value, msg);
            if (send(sock, msg, msg_len, 0) < 0) { ret = ESP_FAIL; goto done; }
            ESP_LOGD(TAG, "Sent purge response: %d", session->last_purge_value);
            break;

        case RFC2217_FLOWCONTROL_SUSPEND:
            msg_len = rfc2217_build_byte_response(RFC2217_RESP_FLOWCONTROL_SUSPEND, 0, msg);
            if (send(sock, msg, msg_len, 0) < 0) { ret = ESP_FAIL; goto done; }
            break;

        case RFC2217_FLOWCONTROL_RESUME:
            msg_len = rfc2217_build_byte_response(RFC2217_RESP_FLOWCONTROL_RESUME, 0, msg);
            if (send(sock, msg, msg_len, 0) < 0) { ret = ESP_FAIL; goto done; }
            break;

        default:
            // No response needed (e.g., for option negotiations WILL/DO/WONT/DONT)
            break;
    }

done:
    session->last_command = 0xFF;
    xSemaphoreGive(s_server.tx_mutex);
    return ret;
}

static esp_err_t apply_serial_settings(rfc2217_session_t *session)
{
    esp_err_t ret = ESP_OK;

    // Only apply line coding if it changed
    if (session->line_coding_changed) {
        serial_line_coding_t coding = {
            .baudrate = session->baudrate,
            .data_bits = session->datasize,
            .parity = 0,
            .stop_bits = 0
        };

        // Map RFC2217 parity values to CDC values
        if (session->parity == RFC2217_PARITY_NONE) {
            coding.parity = 0;
        } else if (session->parity == RFC2217_PARITY_ODD) {
            coding.parity = 1;
        } else if (session->parity == RFC2217_PARITY_EVEN) {
            coding.parity = 2;
        } else if (session->parity == RFC2217_PARITY_MARK) {
            coding.parity = 3;
        } else if (session->parity == RFC2217_PARITY_SPACE) {
            coding.parity = 4;
        }

        // Map RFC2217 stop bits values to CDC values
        if (session->stopsize == RFC2217_STOPSIZE_1) {
            coding.stop_bits = 0;
        } else if (session->stopsize == RFC2217_STOPSIZE_2) {
            coding.stop_bits = 2;
        } else if (session->stopsize == RFC2217_STOPSIZE_15) {
            coding.stop_bits = 1;
        }

        ret = serial_control_set_line_coding(&coding);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set line coding: %s", esp_err_to_name(ret));
        }
        session->line_coding_changed = false;
    }

    // Only apply modem control if it changed
    if (session->modem_control_changed) {
        ret = serial_control_set_modem_control(session->dtr, session->rts);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set modem control: %s", esp_err_to_name(ret));
        }
        session->modem_control_changed = false;
    }

    // Apply break signal if it changed
    if (session->break_changed) {
        esp_err_t brk_ret = serial_control_set_break(session->break_state);
        if (brk_ret != ESP_OK && brk_ret != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Failed to set break: %s", esp_err_to_name(brk_ret));
        }
        session->break_changed = false;
    }

    return ret;
}

// ============================================================================
// Modem status polling task
// ============================================================================

static void rfc2217_modem_poll_task(void *pvParameters)
{
    serial_modem_status_t last_status = {0};
    bool first_poll = true;

    while (s_server.connected && s_server.running) {
        serial_modem_status_t status;
        esp_err_t err = serial_control_get_modem_status(&status);

        if (err == ESP_OK) {
            // Check for changes
            if (first_poll ||
                status.cts != last_status.cts ||
                status.dsr != last_status.dsr ||
                status.ri != last_status.ri ||
                status.cd != last_status.cd) {

                rfc2217_server_notify_modemstate(status.cts, status.dsr,
                                                  status.ri, status.cd);
                last_status = status;
                first_poll = false;
            }
        } else if (err != ESP_ERR_NOT_SUPPORTED) {
            // ESP_ERR_NOT_SUPPORTED is expected for CDC devices
            ESP_LOGW(TAG, "Failed to get modem status: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_RFC2217_MODEM_POLL_INTERVAL_MS));
    }

    vTaskDelete(NULL);
}
