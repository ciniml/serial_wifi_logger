/*
 * SPDX-FileCopyrightText: 2024 Kenta IDA
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFC2217 (Telnet COM Port Control Option) Protocol Implementation
 */

#include "rfc2217_protocol.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "rfc2217_proto";

// ============================================================================
// Session initialization
// ============================================================================

void rfc2217_session_init(rfc2217_session_t *session, const char *signature)
{
    memset(session, 0, sizeof(rfc2217_session_t));

    // Default serial settings
    session->baudrate = 115200;
    session->datasize = RFC2217_DATASIZE_8;
    session->parity = RFC2217_PARITY_NONE;
    session->stopsize = RFC2217_STOPSIZE_1;
    session->flowcontrol = RFC2217_CONTROL_FLOW_NONE;
    session->dtr = false;
    session->rts = false;

    // Default masks (report all changes)
    session->linestate_mask = 0xFF;
    session->modemstate_mask = 0xFF;

    // Set signature
    if (signature != NULL) {
        strncpy(session->signature, signature, RFC2217_SIGNATURE_SIZE - 1);
        session->signature[RFC2217_SIGNATURE_SIZE - 1] = '\0';
    } else {
        strncpy(session->signature, "ESP32-S3 Serial WiFi Logger", RFC2217_SIGNATURE_SIZE - 1);
    }

    session->state = RFC2217_STATE_DATA;
    session->last_command = 0xFF;  // No command pending
    session->last_control_value = 0;
    session->last_purge_value = 0;
    session->need_signature_response = false;
    session->need_option_response = false;
    session->option_response_cmd = 0;
    session->option_response_opt = 0;
    session->break_changed = false;
}

// ============================================================================
// Subnegotiation processing
// ============================================================================

static void process_subnegotiation(rfc2217_session_t *session)
{
    if (session->sb_len < 2) {
        ESP_LOGW(TAG, "Subnegotiation too short: %d bytes", session->sb_len);
        return;
    }

    uint8_t option = session->sb_buffer[0];
    if (option != TELNET_COM_PORT_OPTION) {
        ESP_LOGD(TAG, "Ignoring subnegotiation for option %d", option);
        return;
    }

    uint8_t cmd = session->sb_buffer[1];
    uint8_t *value = &session->sb_buffer[2];
    size_t value_len = session->sb_len - 2;

    ESP_LOGD(TAG, "COM-PORT-OPTION command: %d, value_len: %d", cmd, value_len);

    // Track the command for response
    session->last_command = cmd;

    switch (cmd) {
        case RFC2217_SIGNATURE:
            // Client sending signature or requesting ours
            session->need_signature_response = true;
            ESP_LOGI(TAG, "Received SIGNATURE request");
            break;

        case RFC2217_SET_BAUDRATE:
            if (value_len >= 4) {
                uint32_t baudrate = ((uint32_t)value[0] << 24) |
                                   ((uint32_t)value[1] << 16) |
                                   ((uint32_t)value[2] << 8) |
                                   ((uint32_t)value[3]);
                if (baudrate == 0) {
                    // Request current value
                    ESP_LOGD(TAG, "Baudrate query");
                } else {
                    session->baudrate = baudrate;
                    session->settings_changed = true;
                    session->line_coding_changed = true;
                    ESP_LOGD(TAG, "Set baudrate: %lu", (unsigned long)baudrate);
                }
            }
            break;

        case RFC2217_SET_DATASIZE:
            if (value_len >= 1) {
                if (value[0] == RFC2217_DATASIZE_REQUEST) {
                    ESP_LOGD(TAG, "Datasize query");
                } else if (value[0] >= 5 && value[0] <= 8) {
                    session->datasize = value[0];
                    session->settings_changed = true;
                    session->line_coding_changed = true;
                    ESP_LOGD(TAG, "Set datasize: %d", value[0]);
                }
            }
            break;

        case RFC2217_SET_PARITY:
            if (value_len >= 1) {
                if (value[0] == RFC2217_PARITY_REQUEST) {
                    ESP_LOGD(TAG, "Parity query");
                } else if (value[0] >= RFC2217_PARITY_NONE && value[0] <= RFC2217_PARITY_SPACE) {
                    session->parity = value[0];
                    session->settings_changed = true;
                    session->line_coding_changed = true;
                    ESP_LOGD(TAG, "Set parity: %d", value[0]);
                }
            }
            break;

        case RFC2217_SET_STOPSIZE:
            if (value_len >= 1) {
                if (value[0] == RFC2217_STOPSIZE_REQUEST) {
                    ESP_LOGD(TAG, "Stopsize query");
                } else if (value[0] >= RFC2217_STOPSIZE_1 && value[0] <= RFC2217_STOPSIZE_15) {
                    session->stopsize = value[0];
                    session->settings_changed = true;
                    session->line_coding_changed = true;
                    ESP_LOGD(TAG, "Set stopsize: %d", value[0]);
                }
            }
            break;

        case RFC2217_SET_CONTROL:
            if (value_len >= 1) {
                session->last_control_value = value[0];
                switch (value[0]) {
                    case RFC2217_CONTROL_FLOW_REQUEST:
                    case RFC2217_CONTROL_DTR_REQUEST:
                    case RFC2217_CONTROL_RTS_REQUEST:
                    case RFC2217_CONTROL_BREAK_REQUEST:
                        // Query requests
                        ESP_LOGD(TAG, "Control query: %d", value[0]);
                        break;

                    case RFC2217_CONTROL_FLOW_NONE:
                    case RFC2217_CONTROL_FLOW_XONXOFF:
                    case RFC2217_CONTROL_FLOW_HARDWARE:
                        session->flowcontrol = value[0];
                        session->settings_changed = true;
                        ESP_LOGD(TAG, "Set flow control: %d", value[0]);
                        break;

                    case RFC2217_CONTROL_DTR_ON:
                        session->dtr = true;
                        session->settings_changed = true;
                        session->modem_control_changed = true;
                        ESP_LOGD(TAG, "DTR ON");
                        break;

                    case RFC2217_CONTROL_DTR_OFF:
                        session->dtr = false;
                        session->settings_changed = true;
                        session->modem_control_changed = true;
                        ESP_LOGD(TAG, "DTR OFF");
                        break;

                    case RFC2217_CONTROL_RTS_ON:
                        session->rts = true;
                        session->settings_changed = true;
                        session->modem_control_changed = true;
                        ESP_LOGD(TAG, "RTS ON");
                        break;

                    case RFC2217_CONTROL_RTS_OFF:
                        session->rts = false;
                        session->settings_changed = true;
                        session->modem_control_changed = true;
                        ESP_LOGD(TAG, "RTS OFF");
                        break;

                    case RFC2217_CONTROL_BREAK_ON:
                        session->break_state = true;
                        session->settings_changed = true;
                        session->break_changed = true;
                        ESP_LOGI(TAG, "BREAK ON");
                        break;

                    case RFC2217_CONTROL_BREAK_OFF:
                        session->break_state = false;
                        session->settings_changed = true;
                        session->break_changed = true;
                        ESP_LOGI(TAG, "BREAK OFF");
                        break;

                    default:
                        ESP_LOGW(TAG, "Unknown control value: %d", value[0]);
                        break;
                }
            }
            break;

        case RFC2217_SET_LINESTATE_MASK:
            if (value_len >= 1) {
                session->linestate_mask = value[0];
                ESP_LOGD(TAG, "Set linestate mask: 0x%02X", value[0]);
            }
            break;

        case RFC2217_SET_MODEMSTATE_MASK:
            if (value_len >= 1) {
                session->modemstate_mask = value[0];
                ESP_LOGD(TAG, "Set modemstate mask: 0x%02X", value[0]);
            }
            break;

        case RFC2217_PURGE_DATA:
            if (value_len >= 1) {
                session->last_purge_value = value[0];
                ESP_LOGI(TAG, "Purge data: %d", value[0]);
                // Will be handled by the server
            }
            break;

        case RFC2217_FLOWCONTROL_SUSPEND:
            ESP_LOGD(TAG, "Flow control suspend");
            break;

        case RFC2217_FLOWCONTROL_RESUME:
            ESP_LOGD(TAG, "Flow control resume");
            break;

        default:
            ESP_LOGW(TAG, "Unknown COM-PORT-OPTION command: %d", cmd);
            break;
    }
}

// ============================================================================
// Byte parser (state machine)
// ============================================================================

rfc2217_result_t rfc2217_parse_byte(rfc2217_session_t *session, uint8_t byte,
                                     uint8_t *out_data, size_t *out_len)
{
    *out_len = 0;

    switch (session->state) {
        case RFC2217_STATE_DATA:
            if (byte == TELNET_IAC) {
                session->state = RFC2217_STATE_IAC;
                return RFC2217_RESULT_CONTINUE;
            }
            // Normal data byte
            out_data[0] = byte;
            *out_len = 1;
            return RFC2217_RESULT_DATA;

        case RFC2217_STATE_IAC:
            switch (byte) {
                case TELNET_IAC:
                    // Escaped 0xFF -> output single 0xFF
                    session->state = RFC2217_STATE_DATA;
                    out_data[0] = TELNET_IAC;
                    *out_len = 1;
                    return RFC2217_RESULT_DATA;

                case TELNET_WILL:
                    session->state = RFC2217_STATE_WILL;
                    return RFC2217_RESULT_CONTINUE;

                case TELNET_WONT:
                    session->state = RFC2217_STATE_WONT;
                    return RFC2217_RESULT_CONTINUE;

                case TELNET_DO:
                    session->state = RFC2217_STATE_DO;
                    return RFC2217_RESULT_CONTINUE;

                case TELNET_DONT:
                    session->state = RFC2217_STATE_DONT;
                    return RFC2217_RESULT_CONTINUE;

                case TELNET_SB:
                    session->state = RFC2217_STATE_SB;
                    session->sb_len = 0;
                    return RFC2217_RESULT_CONTINUE;

                case TELNET_NOP:
                case TELNET_GA:
                    session->state = RFC2217_STATE_DATA;
                    return RFC2217_RESULT_CONTINUE;

                default:
                    // Unknown command, ignore
                    ESP_LOGD(TAG, "Unknown IAC command: 0x%02X", byte);
                    session->state = RFC2217_STATE_DATA;
                    return RFC2217_RESULT_CONTINUE;
            }

        case RFC2217_STATE_WILL:
            ESP_LOGD(TAG, "Received WILL for option %d", byte);
            if (byte == TELNET_COM_PORT_OPTION) {
                session->client_will_com_port = true;
                session->com_port_enabled = session->client_do_com_port;
                // No response needed: we already sent DO COM-PORT-OPTION proactively
            } else if (byte == TELNET_OPTION_BINARY || byte == TELNET_OPTION_SGA) {
                // Accept binary/SGA mode: respond DO
                session->need_option_response = true;
                session->option_response_cmd = TELNET_DO;
                session->option_response_opt = byte;
            } else {
                // Unknown option: refuse with DONT
                session->need_option_response = true;
                session->option_response_cmd = TELNET_DONT;
                session->option_response_opt = byte;
            }
            session->state = RFC2217_STATE_DATA;
            return RFC2217_RESULT_COMMAND;

        case RFC2217_STATE_WONT:
            ESP_LOGD(TAG, "Received WONT for option %d", byte);
            if (byte == TELNET_COM_PORT_OPTION) {
                session->client_will_com_port = false;
            }
            // WONT is the end of negotiation, no response required
            session->state = RFC2217_STATE_DATA;
            return RFC2217_RESULT_COMMAND;

        case RFC2217_STATE_DO:
            ESP_LOGD(TAG, "Received DO for option %d", byte);
            if (byte == TELNET_COM_PORT_OPTION) {
                session->client_do_com_port = true;
                session->com_port_enabled = session->client_will_com_port;
                // No response needed: we already sent WILL COM-PORT-OPTION proactively
            } else if (byte == TELNET_OPTION_BINARY || byte == TELNET_OPTION_SGA) {
                // Accept binary/SGA mode: respond WILL
                session->need_option_response = true;
                session->option_response_cmd = TELNET_WILL;
                session->option_response_opt = byte;
            } else {
                // Unknown option: refuse with WONT
                session->need_option_response = true;
                session->option_response_cmd = TELNET_WONT;
                session->option_response_opt = byte;
            }
            session->state = RFC2217_STATE_DATA;
            return RFC2217_RESULT_COMMAND;

        case RFC2217_STATE_DONT:
            ESP_LOGD(TAG, "Received DONT for option %d", byte);
            if (byte == TELNET_COM_PORT_OPTION) {
                session->client_do_com_port = false;
            }
            // DONT is the end of negotiation, no response required
            session->state = RFC2217_STATE_DATA;
            return RFC2217_RESULT_COMMAND;

        case RFC2217_STATE_SB:
            // First byte of subnegotiation (option code)
            if (session->sb_len < RFC2217_SB_BUFFER_SIZE) {
                session->sb_buffer[session->sb_len++] = byte;
            }
            session->state = RFC2217_STATE_SB_DATA;
            return RFC2217_RESULT_CONTINUE;

        case RFC2217_STATE_SB_DATA:
            if (byte == TELNET_IAC) {
                session->state = RFC2217_STATE_SB_IAC;
                return RFC2217_RESULT_CONTINUE;
            }
            // Collect subnegotiation data
            if (session->sb_len < RFC2217_SB_BUFFER_SIZE) {
                session->sb_buffer[session->sb_len++] = byte;
            }
            return RFC2217_RESULT_CONTINUE;

        case RFC2217_STATE_SB_IAC:
            if (byte == TELNET_SE) {
                // End of subnegotiation
                process_subnegotiation(session);
                session->state = RFC2217_STATE_DATA;
                return RFC2217_RESULT_COMMAND;
            } else if (byte == TELNET_IAC) {
                // Escaped 0xFF in subnegotiation
                if (session->sb_len < RFC2217_SB_BUFFER_SIZE) {
                    session->sb_buffer[session->sb_len++] = TELNET_IAC;
                }
                session->state = RFC2217_STATE_SB_DATA;
                return RFC2217_RESULT_CONTINUE;
            } else {
                // Unexpected, treat as end of subnegotiation
                ESP_LOGW(TAG, "Unexpected byte after IAC in SB: 0x%02X", byte);
                session->state = RFC2217_STATE_DATA;
                return RFC2217_RESULT_ERROR;
            }

        default:
            session->state = RFC2217_STATE_DATA;
            return RFC2217_RESULT_ERROR;
    }
}

// ============================================================================
// Data escaping for transmission
// ============================================================================

size_t rfc2217_escape_data(const uint8_t *in, size_t in_len,
                           uint8_t *out, size_t out_size)
{
    size_t j = 0;
    for (size_t i = 0; i < in_len && j < out_size - 1; i++) {
        if (in[i] == TELNET_IAC) {
            out[j++] = TELNET_IAC;
            if (j < out_size) {
                out[j++] = TELNET_IAC;
            }
        } else {
            out[j++] = in[i];
        }
    }
    return j;
}

// ============================================================================
// Response builders
// ============================================================================

size_t rfc2217_build_will_com_port(uint8_t *out)
{
    out[0] = TELNET_IAC;
    out[1] = TELNET_WILL;
    out[2] = TELNET_COM_PORT_OPTION;
    return 3;
}

size_t rfc2217_build_do_com_port(uint8_t *out)
{
    out[0] = TELNET_IAC;
    out[1] = TELNET_DO;
    out[2] = TELNET_COM_PORT_OPTION;
    return 3;
}

size_t rfc2217_build_subneg(uint8_t cmd, const uint8_t *value,
                            size_t value_len, uint8_t *out)
{
    size_t pos = 0;
    out[pos++] = TELNET_IAC;
    out[pos++] = TELNET_SB;
    out[pos++] = TELNET_COM_PORT_OPTION;
    out[pos++] = cmd;

    // Copy value with IAC escaping
    for (size_t i = 0; i < value_len; i++) {
        if (value[i] == TELNET_IAC) {
            out[pos++] = TELNET_IAC;
        }
        out[pos++] = value[i];
    }

    out[pos++] = TELNET_IAC;
    out[pos++] = TELNET_SE;
    return pos;
}

size_t rfc2217_build_baudrate_response(uint32_t baudrate, uint8_t *out)
{
    uint8_t value[4];
    value[0] = (baudrate >> 24) & 0xFF;
    value[1] = (baudrate >> 16) & 0xFF;
    value[2] = (baudrate >> 8) & 0xFF;
    value[3] = baudrate & 0xFF;
    return rfc2217_build_subneg(RFC2217_RESP_SET_BAUDRATE, value, 4, out);
}

size_t rfc2217_build_byte_response(uint8_t cmd, uint8_t value, uint8_t *out)
{
    return rfc2217_build_subneg(cmd, &value, 1, out);
}

size_t rfc2217_build_modemstate(uint8_t state, uint8_t *out)
{
    return rfc2217_build_subneg(RFC2217_RESP_NOTIFY_MODEMSTATE, &state, 1, out);
}

size_t rfc2217_build_linestate(uint8_t state, uint8_t *out)
{
    return rfc2217_build_subneg(RFC2217_RESP_NOTIFY_LINESTATE, &state, 1, out);
}

size_t rfc2217_build_signature(const char *signature, uint8_t *out)
{
    size_t sig_len = strlen(signature);
    return rfc2217_build_subneg(RFC2217_RESP_SIGNATURE, (const uint8_t *)signature, sig_len, out);
}
