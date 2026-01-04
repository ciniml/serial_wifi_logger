| Supported Targets | ESP32-S3 |
| ----------------- | -------- |

# USB Serial Host Driver with Auto-Detection

This project demonstrates USB serial communication on ESP32-S3 with support for both CDC-ACM and FTDI devices. The application can automatically detect and switch between different USB serial devices at runtime based on their VID/PID.

## Features

- **CDC-ACM Driver Support**: Standard USB Communications Device Class for serial devices
- **FTDI Driver Support**: FTDI-specific USB-to-serial chips (FT232R, FT2232, etc.)
- **Automatic Device Detection**: Runtime detection and driver selection based on USB VID/PID
- **Three Operating Modes**:
  - **CDC-ACM Only**: Use only the CDC-ACM driver
  - **FTDI Only**: Use only the FTDI driver
  - **Auto-Detect**: Both drivers installed, automatic switching based on connected device

## How Auto-Detection Works

The auto-detection mechanism uses VID-based device classification:

- **FTDI Devices**: VID = `0x0403` (standard FTDI Vendor ID)
  - Automatically handled by FTDI driver
  - Supports FT232R, FT2232H, FT4232H, FT232H, and other FTDI chips

- **CDC Devices**: All other VIDs
  - Handled by CDC-ACM driver
  - Supports standard CDC-ACM compatible devices

When a USB device is connected:
1. Both drivers receive new device notifications
2. Each driver checks the device VID/PID
3. The FTDI driver handles devices with VID `0x0403`
4. The CDC driver handles all other devices
5. The application automatically uses the appropriate driver

## Hardware Required

- **ESP32-S3 Development Board** with USB OTG support
- **USB Cable** for connecting to PC (programming and monitoring)
- **USB Serial Device** (one or more):
  - CDC-ACM device (e.g., another ESP32-S3 running TinyUSB CDC example)
  - FTDI device (e.g., FT232R USB-to-serial adapter)

### Pin Assignment

The ESP32-S3 uses internal USB OTG pins:
- **GPIO 19**: USB D- (negative data line)
- **GPIO 20**: USB D+ (positive data line)

No external pin configuration required for USB OTG.

## Build and Flash

### 1. Configure the Project

Select the desired USB serial driver mode:

```bash
idf.py menuconfig
```

Navigate to: **USB Serial Configuration → USB Serial Driver Type**

Choose one of:
- **CDC-ACM Driver**: Use only CDC-ACM driver
- **FTDI Driver**: Use only FTDI driver
- **Auto-Detect (Both Drivers)**: Recommended - automatically switch between drivers

### 2. Build the Project

```bash
idf.py build
```

### 3. Flash to ESP32-S3

```bash
idf.py -p PORT flash monitor
```

Replace `PORT` with your serial port (e.g., `/dev/ttyUSB0` on Linux, `COM3` on Windows).

To exit the serial monitor, type `Ctrl-]`.

## Example Output

### CDC-ACM Device Connected

```
I (256) USB-SERIAL-AUTO: USB Host installed
I (256) USB-SERIAL-AUTO: CDC-ACM driver installed
I (256) USB-SERIAL-AUTO: FTDI driver installed
I (356) USB-SERIAL-AUTO: CDC device detected: VID=0x303A PID=0x4001
I (456) USB-SERIAL-AUTO: Opening CDC device 0x303A:0x4001
I (556) USB-SERIAL-AUTO: [CDC] Device opened successfully
I (656) USB-SERIAL-AUTO: [CDC] Setting line coding: 115200 8N1
I (756) USB-SERIAL-AUTO: [CDC] Data received (3 bytes)
I (756) USB-SERIAL-AUTO: 41 54 0d                                          |AT.|
```

### FTDI Device Connected

```
I (256) USB-SERIAL-AUTO: USB Host installed
I (256) USB-SERIAL-AUTO: CDC-ACM driver installed
I (256) USB-SERIAL-AUTO: FTDI driver installed
I (356) USB-SERIAL-AUTO: FTDI device detected: VID=0x0403 PID=0x6001
I (456) USB-SERIAL-AUTO: Opening FTDI device 0x0403:0x6001
I (556) USB-SERIAL-AUTO: [FTDI] Device opened successfully
I (656) USB-SERIAL-AUTO: [FTDI] Device type: FT232R
I (756) USB-SERIAL-AUTO: [FTDI] Setting baudrate: 115200
I (856) USB-SERIAL-AUTO: [FTDI] Setting line property: 8N1
I (956) USB-SERIAL-AUTO: [FTDI] Data received (3 bytes)
I (956) USB-SERIAL-AUTO: 41 54 0d                                          |AT.|
```

### Device Switching

You can connect and disconnect different USB serial devices, and the application will automatically:
1. Detect the device type (CDC or FTDI)
2. Open the device with the appropriate driver
3. Configure serial parameters (baudrate, data bits, parity, stop bits)
4. Receive and display data
5. Close the device when disconnected
6. Wait for the next device

## Architecture

### Application Structure

```
usb_serial_auto_main.c (Auto-Detect Mode)
├── USB Host Library (shared by both drivers)
├── CDC-ACM Host Driver
│   ├── new_dev_cb: Detects non-FTDI devices (VID != 0x0403)
│   ├── data_cb: Receives data from CDC devices
│   └── event_cb: Handles CDC device events
└── FTDI Host Driver
    ├── new_dev_cb: Detects FTDI devices (VID == 0x0403)
    ├── data_cb: Receives data from FTDI devices
    └── event_cb: Handles FTDI device events
```

### Device Queue Mechanism

The application uses a FreeRTOS queue to manage device detection:
1. Both drivers register `new_dev_cb` callbacks
2. When a device is detected, the appropriate driver adds it to the queue
3. The main task dequeues devices and handles them sequentially
4. Each device gets a semaphore for disconnection signaling

### Callback Wrapper Functions

CDC and FTDI drivers have incompatible callback signatures:
- **CDC data callback**: Returns `bool` (data processed indicator)
- **FTDI data callback**: Returns `void`
- **CDC event callback**: Receives `cdc_acm_host_dev_event_data_t *`
- **FTDI event callback**: Receives `ftdi_sio_host_dev_event_t` enum

The application implements 4 separate wrapper functions to handle these differences.

## Components

### FTDI SIO Host Driver

Custom component located at `components/usb_host_ftdi_sio/`:
- **Protocol Layer**: FTDI-specific USB control requests (baudrate, line properties, modem control)
- **Host Driver**: USB device management, bulk transfers, event handling
- **Descriptor Parsing**: FTDI chip type detection (FT232R, FT2232H, etc.)

See [components/usb_host_ftdi_sio/README.md](components/usb_host_ftdi_sio/README.md) for details.

### CDC-ACM Host Driver

Uses the ESP-IDF managed component: `espressif__usb_host_cdc_acm`
- Standard USB CDC-ACM protocol
- Line coding configuration
- Control line state (DTR/RTS)

## Supported Devices

### CDC-ACM Devices
- ESP32-S2/S3 running TinyUSB CDC example
- Arduino boards with native USB (Leonardo, Micro, etc.)
- Most USB modems and virtual COM ports
- Any USB device implementing CDC-ACM protocol

### FTDI Devices
- FT232R (single port)
- FT2232H (dual port)
- FT4232H (quad port)
- FT232H (high-speed single port)
- Other FTDI chips with VID `0x0403`

## Limitations

- **Sequential Device Handling**: Only one device is processed at a time
- **No Multi-Interface Support**: Multi-port FTDI devices currently use only the first interface
- **VID 0x0403 Priority**: Devices with FTDI VID are always routed to FTDI driver, even if they implement CDC-ACM

## Troubleshooting

### Build Errors

**Problem**: `fatal error: usb/ftdi_sio_host.h: No such file or directory`

**Solution**: Ensure you've selected "Auto-Detect (Both Drivers)" in menuconfig, or manually enable:
```
CONFIG_USB_SERIAL_DRIVER_AUTO=y
```

### Device Not Detected

**Problem**: No device detection logs appear

**Solutions**:
1. Check USB cable connection
2. Verify ESP32-S3 USB OTG is enabled
3. Check USB device is functioning (test on PC)
4. Enable debug logs: `idf.py menuconfig` → Component config → Log output → Default log verbosity → Debug

### Wrong Driver Selected

**Problem**: FTDI device handled by CDC driver (or vice versa)

**Solutions**:
1. Verify device VID with `lsusb` on Linux or Device Manager on Windows
2. If FTDI device has non-standard VID, modify detection logic in `usb_serial_auto_main.c`
3. Check driver installation logs to confirm both drivers are loaded

## License

This project is licensed under the Apache License 2.0.

See individual component READMEs for component-specific licenses.

## References

- [ESP-IDF USB Host Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_host.html)
- [USB CDC-ACM Specification](https://www.usb.org/document-library/class-definitions-communication-devices-12)
- [FTDI Chip Datasheets](https://ftdichip.com/product-category/products/ic/)
- [ESP32-S3 USB OTG](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_host.html)
