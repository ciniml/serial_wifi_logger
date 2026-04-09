/*
 * SPDX-FileCopyrightText: 2024 Kenta IDA
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFC2217 (Telnet COM Port Control Option) Protocol Definitions
 */

#ifndef RFC2217_PROTOCOL_H
#define RFC2217_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Telnet Basic Codes
// ============================================================================

#define TELNET_IAC          0xFF    // Interpret As Command
#define TELNET_DONT         0xFE
#define TELNET_DO           0xFD
#define TELNET_WONT         0xFC
#define TELNET_WILL         0xFB
#define TELNET_SB           0xFA    // Subnegotiation Begin
#define TELNET_GA           0xF9    // Go Ahead
#define TELNET_EL           0xF8    // Erase Line
#define TELNET_EC           0xF7    // Erase Character
#define TELNET_AYT          0xF6    // Are You There
#define TELNET_AO           0xF5    // Abort Output
#define TELNET_IP           0xF4    // Interrupt Process
#define TELNET_BRK          0xF3    // Break
#define TELNET_DM           0xF2    // Data Mark
#define TELNET_NOP          0xF1    // No Operation
#define TELNET_SE           0xF0    // Subnegotiation End

// ============================================================================
// Telnet Option Codes
// ============================================================================

#define TELNET_OPTION_BINARY    0   // Binary Transmission (RFC856)
#define TELNET_OPTION_ECHO      1   // Echo (RFC857)
#define TELNET_OPTION_SGA       3   // Suppress Go Ahead (RFC858)

// ============================================================================
// COM-PORT-OPTION (RFC2217)
// ============================================================================

#define TELNET_COM_PORT_OPTION  44

// Client -> Server commands (0-12)
#define RFC2217_SIGNATURE               0
#define RFC2217_SET_BAUDRATE            1
#define RFC2217_SET_DATASIZE            2
#define RFC2217_SET_PARITY              3
#define RFC2217_SET_STOPSIZE            4
#define RFC2217_SET_CONTROL             5
#define RFC2217_NOTIFY_LINESTATE        6
#define RFC2217_NOTIFY_MODEMSTATE       7
#define RFC2217_FLOWCONTROL_SUSPEND     8
#define RFC2217_FLOWCONTROL_RESUME      9
#define RFC2217_SET_LINESTATE_MASK      10
#define RFC2217_SET_MODEMSTATE_MASK     11
#define RFC2217_PURGE_DATA              12

// Server -> Client responses (commands + 100)
#define RFC2217_RESP_OFFSET             100
#define RFC2217_RESP_SIGNATURE          (RFC2217_SIGNATURE + RFC2217_RESP_OFFSET)
#define RFC2217_RESP_SET_BAUDRATE       (RFC2217_SET_BAUDRATE + RFC2217_RESP_OFFSET)
#define RFC2217_RESP_SET_DATASIZE       (RFC2217_SET_DATASIZE + RFC2217_RESP_OFFSET)
#define RFC2217_RESP_SET_PARITY         (RFC2217_SET_PARITY + RFC2217_RESP_OFFSET)
#define RFC2217_RESP_SET_STOPSIZE       (RFC2217_SET_STOPSIZE + RFC2217_RESP_OFFSET)
#define RFC2217_RESP_SET_CONTROL        (RFC2217_SET_CONTROL + RFC2217_RESP_OFFSET)
#define RFC2217_RESP_NOTIFY_LINESTATE   (RFC2217_NOTIFY_LINESTATE + RFC2217_RESP_OFFSET)
#define RFC2217_RESP_NOTIFY_MODEMSTATE  (RFC2217_NOTIFY_MODEMSTATE + RFC2217_RESP_OFFSET)
#define RFC2217_RESP_FLOWCONTROL_SUSPEND (RFC2217_FLOWCONTROL_SUSPEND + RFC2217_RESP_OFFSET)
#define RFC2217_RESP_FLOWCONTROL_RESUME (RFC2217_FLOWCONTROL_RESUME + RFC2217_RESP_OFFSET)
#define RFC2217_RESP_SET_LINESTATE_MASK (RFC2217_SET_LINESTATE_MASK + RFC2217_RESP_OFFSET)
#define RFC2217_RESP_SET_MODEMSTATE_MASK (RFC2217_SET_MODEMSTATE_MASK + RFC2217_RESP_OFFSET)
#define RFC2217_RESP_PURGE_DATA         (RFC2217_PURGE_DATA + RFC2217_RESP_OFFSET)

// ============================================================================
// SET-DATASIZE values
// ============================================================================

#define RFC2217_DATASIZE_REQUEST    0   // Request current value
#define RFC2217_DATASIZE_5          5
#define RFC2217_DATASIZE_6          6
#define RFC2217_DATASIZE_7          7
#define RFC2217_DATASIZE_8          8

// ============================================================================
// SET-PARITY values
// ============================================================================

#define RFC2217_PARITY_REQUEST      0   // Request current value
#define RFC2217_PARITY_NONE         1
#define RFC2217_PARITY_ODD          2
#define RFC2217_PARITY_EVEN         3
#define RFC2217_PARITY_MARK         4
#define RFC2217_PARITY_SPACE        5

// ============================================================================
// SET-STOPSIZE values
// ============================================================================

#define RFC2217_STOPSIZE_REQUEST    0   // Request current value
#define RFC2217_STOPSIZE_1          1   // 1 stop bit
#define RFC2217_STOPSIZE_2          2   // 2 stop bits
#define RFC2217_STOPSIZE_15         3   // 1.5 stop bits

// ============================================================================
// SET-CONTROL values (flow control and signals)
// ============================================================================

// Flow control settings (1-3)
#define RFC2217_CONTROL_FLOW_REQUEST        0   // Request current value
#define RFC2217_CONTROL_FLOW_NONE           1
#define RFC2217_CONTROL_FLOW_XONXOFF        2
#define RFC2217_CONTROL_FLOW_HARDWARE       3

// Break signal (4-6)
#define RFC2217_CONTROL_BREAK_REQUEST       4   // Request current state
#define RFC2217_CONTROL_BREAK_ON            5
#define RFC2217_CONTROL_BREAK_OFF           6

// DTR signal (7-9)
#define RFC2217_CONTROL_DTR_REQUEST         7   // Request current state
#define RFC2217_CONTROL_DTR_ON              8
#define RFC2217_CONTROL_DTR_OFF             9

// RTS signal (10-12)
#define RFC2217_CONTROL_RTS_REQUEST         10  // Request current state
#define RFC2217_CONTROL_RTS_ON              11
#define RFC2217_CONTROL_RTS_OFF             12

// Inbound flow control (13-16)
#define RFC2217_CONTROL_INFLOW_REQUEST      13
#define RFC2217_CONTROL_INFLOW_NONE         14
#define RFC2217_CONTROL_INFLOW_XONXOFF      15
#define RFC2217_CONTROL_INFLOW_HARDWARE     16

// ============================================================================
// NOTIFY-MODEMSTATE bit definitions
// ============================================================================

#define RFC2217_MODEMSTATE_CD           0x80    // Carrier Detect (RLSD)
#define RFC2217_MODEMSTATE_RI           0x40    // Ring Indicator
#define RFC2217_MODEMSTATE_DSR          0x20    // Data Set Ready
#define RFC2217_MODEMSTATE_CTS          0x10    // Clear To Send
#define RFC2217_MODEMSTATE_DELTA_CD     0x08    // Delta Carrier Detect
#define RFC2217_MODEMSTATE_DELTA_RI     0x04    // Trailing edge Ring Indicator
#define RFC2217_MODEMSTATE_DELTA_DSR    0x02    // Delta DSR
#define RFC2217_MODEMSTATE_DELTA_CTS    0x01    // Delta CTS

// ============================================================================
// NOTIFY-LINESTATE bit definitions
// ============================================================================

#define RFC2217_LINESTATE_TIMEOUT       0x80    // Timeout
#define RFC2217_LINESTATE_TSRE          0x40    // Transfer Shift Register Empty
#define RFC2217_LINESTATE_THRE          0x20    // Transfer Holding Register Empty
#define RFC2217_LINESTATE_BREAK         0x10    // Break Detect
#define RFC2217_LINESTATE_FRAMING       0x08    // Framing Error
#define RFC2217_LINESTATE_PARITY        0x04    // Parity Error
#define RFC2217_LINESTATE_OVERRUN       0x02    // Overrun Error
#define RFC2217_LINESTATE_DATA_READY    0x01    // Data Ready

// ============================================================================
// PURGE-DATA values
// ============================================================================

#define RFC2217_PURGE_RX        1   // Purge receive buffer
#define RFC2217_PURGE_TX        2   // Purge transmit buffer
#define RFC2217_PURGE_BOTH      3   // Purge both buffers

// ============================================================================
// Parser state machine
// ============================================================================

typedef enum {
    RFC2217_STATE_DATA,         // Normal data mode
    RFC2217_STATE_IAC,          // Received IAC
    RFC2217_STATE_WILL,         // Received IAC WILL
    RFC2217_STATE_WONT,         // Received IAC WONT
    RFC2217_STATE_DO,           // Received IAC DO
    RFC2217_STATE_DONT,         // Received IAC DONT
    RFC2217_STATE_SB,           // Received IAC SB
    RFC2217_STATE_SB_DATA,      // In subnegotiation, collecting data
    RFC2217_STATE_SB_IAC,       // In subnegotiation, received IAC
} rfc2217_parser_state_t;

// ============================================================================
// Session state structure
// ============================================================================

#define RFC2217_SB_BUFFER_SIZE  64
#define RFC2217_SIGNATURE_SIZE  64

typedef struct {
    // Parser state
    rfc2217_parser_state_t state;

    // Subnegotiation buffer
    uint8_t sb_buffer[RFC2217_SB_BUFFER_SIZE];
    size_t sb_len;

    // Option negotiation state
    bool com_port_enabled;      // COM-PORT-OPTION negotiated
    bool client_will_com_port;  // Client sent WILL COM-PORT-OPTION
    bool client_do_com_port;    // Client sent DO COM-PORT-OPTION

    // Current serial port settings
    uint32_t baudrate;
    uint8_t datasize;           // 5-8
    uint8_t parity;             // RFC2217_PARITY_*
    uint8_t stopsize;           // RFC2217_STOPSIZE_*
    uint8_t flowcontrol;        // RFC2217_CONTROL_FLOW_*
    bool dtr;
    bool rts;
    bool break_state;

    // Mask for notifications
    uint8_t linestate_mask;
    uint8_t modemstate_mask;

    // Last known states (for delta detection)
    uint8_t last_linestate;
    uint8_t last_modemstate;

    // Server signature
    char signature[RFC2217_SIGNATURE_SIZE];

    // Flags for pending actions
    bool settings_changed;
    bool line_coding_changed;   // baudrate/datasize/parity/stopsize changed
    bool modem_control_changed; // DTR/RTS changed
    bool send_modemstate;
    bool send_linestate;

    // Last processed command (for response tracking)
    uint8_t last_command;           // Last RFC2217_* command processed
    uint8_t last_control_value;     // Last SET_CONTROL value (for response)
    uint8_t last_purge_value;       // Last PURGE_DATA value (for response)
    bool need_signature_response;   // Signature response pending

    // Pending Telnet option negotiation response (WILL/WONT/DO/DONT)
    bool need_option_response;
    uint8_t option_response_cmd;    // TELNET_WILL/WONT/DO/DONT
    uint8_t option_response_opt;    // Telnet option code

    // Break signal change tracking
    bool break_changed;
} rfc2217_session_t;

// ============================================================================
// Parse result
// ============================================================================

typedef enum {
    RFC2217_RESULT_DATA,        // Output contains data to forward
    RFC2217_RESULT_CONTINUE,    // No output, continue parsing
    RFC2217_RESULT_COMMAND,     // Command processed, may need response
    RFC2217_RESULT_ERROR,       // Parse error
} rfc2217_result_t;

// ============================================================================
// Function prototypes
// ============================================================================

/**
 * @brief Initialize RFC2217 session state
 * @param session Pointer to session structure
 * @param signature Server signature string (optional, can be NULL)
 */
void rfc2217_session_init(rfc2217_session_t *session, const char *signature);

/**
 * @brief Parse a single byte from the input stream
 * @param session Pointer to session structure
 * @param byte Input byte to parse
 * @param out_data Output buffer for data (when result is RFC2217_RESULT_DATA)
 * @param out_len Pointer to output data length
 * @return Parse result indicating what to do next
 */
rfc2217_result_t rfc2217_parse_byte(rfc2217_session_t *session, uint8_t byte,
                                     uint8_t *out_data, size_t *out_len);

/**
 * @brief Escape data for transmission (double IAC bytes)
 * @param in Input data buffer
 * @param in_len Input data length
 * @param out Output buffer (must be at least 2x in_len)
 * @param out_size Output buffer size
 * @return Number of bytes written to output
 */
size_t rfc2217_escape_data(const uint8_t *in, size_t in_len,
                           uint8_t *out, size_t out_size);

/**
 * @brief Build WILL COM-PORT-OPTION response
 * @param out Output buffer
 * @return Number of bytes written
 */
size_t rfc2217_build_will_com_port(uint8_t *out);

/**
 * @brief Build DO COM-PORT-OPTION request
 * @param out Output buffer
 * @return Number of bytes written
 */
size_t rfc2217_build_do_com_port(uint8_t *out);

/**
 * @brief Build subnegotiation response
 * @param cmd Command code (with RFC2217_RESP_OFFSET)
 * @param value Value bytes
 * @param value_len Value length
 * @param out Output buffer
 * @return Number of bytes written
 */
size_t rfc2217_build_subneg(uint8_t cmd, const uint8_t *value,
                            size_t value_len, uint8_t *out);

/**
 * @brief Build baudrate response
 * @param baudrate Baudrate value
 * @param out Output buffer
 * @return Number of bytes written
 */
size_t rfc2217_build_baudrate_response(uint32_t baudrate, uint8_t *out);

/**
 * @brief Build single-byte response (datasize, parity, stopsize, control)
 * @param cmd Response command code
 * @param value Single byte value
 * @param out Output buffer
 * @return Number of bytes written
 */
size_t rfc2217_build_byte_response(uint8_t cmd, uint8_t value, uint8_t *out);

/**
 * @brief Build modemstate notification
 * @param state Modemstate bits
 * @param out Output buffer
 * @return Number of bytes written
 */
size_t rfc2217_build_modemstate(uint8_t state, uint8_t *out);

/**
 * @brief Build linestate notification
 * @param state Linestate bits
 * @param out Output buffer
 * @return Number of bytes written
 */
size_t rfc2217_build_linestate(uint8_t state, uint8_t *out);

/**
 * @brief Build signature response
 * @param signature Signature string
 * @param out Output buffer
 * @return Number of bytes written
 */
size_t rfc2217_build_signature(const char *signature, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif // RFC2217_PROTOCOL_H
