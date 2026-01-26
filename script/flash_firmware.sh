#!/bin/bash
#
# Flash Firmware Script
# Downloads firmware from GitHub releases and flashes to ESP32-S3 using esptool.py
#
# Usage:
#   ./flash_firmware.sh [version] [port] [baud]
#   ./flash_firmware.sh v1.0.0 /dev/ttyUSB0 460800
#   ./flash_firmware.sh latest /dev/ttyUSB0
#   ./flash_firmware.sh  # Uses latest version, auto-detect port, 460800 baud
#

set -e

# Enable debug mode with DEBUG=1 environment variable
if [ "${DEBUG:-0}" = "1" ]; then
    set -x
fi

# Script version
SCRIPT_VERSION="1.0.0"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# GitHub repository information
GITHUB_REPO="ciniml/serial_wifi_logger"
GITHUB_API="https://api.github.com/repos/${GITHUB_REPO}"

# Temporary directory for downloads
TMP_DIR="/tmp/flash_firmware_$$"

# ESP32-S3 chip configuration
CHIP="esp32s3"
DEFAULT_BAUD=460800
FLASH_OFFSET="0x0"

print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

cleanup() {
    if [ -d "$TMP_DIR" ]; then
        print_info "Cleaning up temporary files..."
        rm -rf "$TMP_DIR"
    fi
}

trap cleanup EXIT

usage() {
    cat << EOF
Usage: $0 [version] [port] [baud]

Arguments:
  version   GitHub release version (e.g., v1.0.0) or "latest" (default: latest)
  port      Serial port for ESP32-S3 (e.g., /dev/ttyUSB0, COM3) (default: auto-detect)
  baud      Baud rate for flashing (default: 460800)

Examples:
  $0
  $0 v1.0.0
  $0 v1.0.0 /dev/ttyUSB0
  $0 latest /dev/ttyUSB0 921600
  $0 v0.2.0 COM3 460800

Environment Variables:
  GITHUB_TOKEN   Optional GitHub personal access token for private repos
  ESPTOOL        Path to esptool.py (default: esptool.py or esptool)

Note:
  This script requires esptool.py to be installed.
  Install with: pip install esptool
EOF
    exit 1
}

check_dependencies() {
    local missing_deps=()

    for cmd in curl jq unzip; do
        if ! command -v "$cmd" &> /dev/null; then
            missing_deps+=("$cmd")
        fi
    done

    if [ ${#missing_deps[@]} -ne 0 ]; then
        print_error "Missing required dependencies: ${missing_deps[*]}"
        print_info "Install them with:"
        print_info "  Ubuntu/Debian: sudo apt-get install curl jq unzip"
        print_info "  macOS: brew install curl jq unzip"
        exit 1
    fi

    # Check for esptool.py
    local esptool_cmd="${ESPTOOL:-}"
    if [ -z "$esptool_cmd" ]; then
        if command -v esptool.py &> /dev/null; then
            esptool_cmd="esptool.py"
        elif command -v esptool &> /dev/null; then
            esptool_cmd="esptool"
        else
            print_error "esptool.py not found"
            print_info "Install with: pip install esptool"
            exit 1
        fi
    fi

    ESPTOOL="$esptool_cmd"
    print_info "Using esptool: $ESPTOOL"
}

get_latest_release() {
    print_info "Fetching latest release information from GitHub..." >&2

    local curl_opts=(-s -f)
    if [ -n "$GITHUB_TOKEN" ]; then
        curl_opts+=(-H "Authorization: token $GITHUB_TOKEN")
    fi

    local release_info=$(curl "${curl_opts[@]}" "$GITHUB_API/releases/latest" 2>&1)
    local curl_exit=$?

    if [ $curl_exit -ne 0 ]; then
        print_error "Failed to fetch release information from GitHub" >&2
        print_error "curl exit code: $curl_exit" >&2
        print_info "API URL: $GITHUB_API/releases/latest" >&2
        print_info "Make sure the repository exists and has releases" >&2
        exit 1
    fi

    if ! echo "$release_info" | jq -e . &> /dev/null; then
        print_error "Invalid JSON response from GitHub API" >&2
        print_error "Response: $release_info" >&2
        exit 1
    fi

    if echo "$release_info" | jq -e '.message' &> /dev/null; then
        local error_msg=$(echo "$release_info" | jq -r '.message')
        print_error "GitHub API error: $error_msg" >&2
        exit 1
    fi

    echo "$release_info"
}

get_release_by_tag() {
    local tag="$1"
    print_info "Fetching release information for tag: $tag" >&2

    local curl_opts=(-s -f)
    if [ -n "$GITHUB_TOKEN" ]; then
        curl_opts+=(-H "Authorization: token $GITHUB_TOKEN")
    fi

    local release_info=$(curl "${curl_opts[@]}" "$GITHUB_API/releases/tags/$tag" 2>&1)
    local curl_exit=$?

    if [ $curl_exit -ne 0 ]; then
        print_error "Failed to fetch release information from GitHub" >&2
        print_error "curl exit code: $curl_exit" >&2
        print_info "API URL: $GITHUB_API/releases/tags/$tag" >&2
        print_error "Release tag '$tag' not found or repository does not exist" >&2
        exit 1
    fi

    if ! echo "$release_info" | jq -e . &> /dev/null; then
        print_error "Invalid JSON response from GitHub API" >&2
        print_error "Response: $release_info" >&2
        exit 1
    fi

    if echo "$release_info" | jq -e '.message' &> /dev/null; then
        local error_msg=$(echo "$release_info" | jq -r '.message')
        print_error "GitHub API error: $error_msg" >&2
        print_error "Release tag '$tag' not found" >&2
        exit 1
    fi

    echo "$release_info"
}

download_firmware() {
    local release_info="$1"

    local tag_name=$(echo "$release_info" | jq -r '.tag_name')
    local download_url=$(echo "$release_info" | jq -r '.assets[] | select(.name | endswith(".zip")) | .browser_download_url' | head -n 1)

    if [ -z "$download_url" ] || [ "$download_url" = "null" ]; then
        print_error "No firmware ZIP file found in release $tag_name" >&2
        exit 1
    fi

    local zip_file="$TMP_DIR/firmware.zip"

    print_info "Downloading firmware from: $download_url" >&2
    mkdir -p "$TMP_DIR"

    local curl_opts=(-L -o "$zip_file" --progress-bar)
    if [ -n "$GITHUB_TOKEN" ]; then
        curl_opts+=(-H "Authorization: token $GITHUB_TOKEN")
    fi

    if ! curl "${curl_opts[@]}" "$download_url" 2>&2; then
        print_error "Failed to download firmware" >&2
        exit 1
    fi

    print_success "Downloaded firmware ZIP: $(du -h "$zip_file" | cut -f1)" >&2

    # Extract firmware
    print_info "Extracting firmware..." >&2
    unzip -q "$zip_file" -d "$TMP_DIR" >&2

    # Look for the single flash image
    local firmware_bin="$TMP_DIR/firmware-${tag_name}.bin"

    if [ ! -f "$firmware_bin" ]; then
        print_error "Firmware binary not found in ZIP: firmware-${tag_name}.bin" >&2
        print_info "Available files in ZIP:" >&2
        unzip -l "$zip_file" >&2
        exit 1
    fi

    print_success "Extracted firmware: $(du -h "$firmware_bin" | cut -f1)" >&2
    echo "$firmware_bin"
}

detect_port() {
    print_info "Auto-detecting ESP32-S3 port..." >&2

    # Try common serial port patterns
    local ports=()

    # Linux/macOS patterns
    for port in /dev/ttyUSB* /dev/ttyACM* /dev/cu.usbserial* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial*; do
        if [ -e "$port" ]; then
            ports+=("$port")
        fi
    done

    if [ ${#ports[@]} -eq 0 ]; then
        print_error "No serial ports detected" >&2
        print_info "Available ports:" >&2
        ls -la /dev/tty* 2>&1 | grep -E "(USB|ACM|serial)" >&2 || true
        print_info "Please specify the port manually" >&2
        exit 1
    fi

    if [ ${#ports[@]} -eq 1 ]; then
        print_info "Detected port: ${ports[0]}" >&2
        echo "${ports[0]}"
        return 0
    fi

    # Multiple ports found, try to identify ESP32-S3
    print_warning "Multiple serial ports detected:" >&2
    for i in "${!ports[@]}"; do
        echo "  $((i+1)). ${ports[$i]}" >&2
    done

    # Use the first one
    print_info "Using first port: ${ports[0]}" >&2
    print_info "If this is incorrect, specify the port manually" >&2
    echo "${ports[0]}"
}

flash_firmware() {
    local firmware_bin="$1"
    local port="$2"
    local baud="$3"

    print_info "Flashing firmware to ESP32-S3..."
    print_info "Port: $port"
    print_info "Baud rate: $baud"
    print_info "Firmware: $firmware_bin ($(du -h "$firmware_bin" | cut -f1))"
    echo ""

    print_warning "Make sure ESP32-S3 is in download mode (press and hold BOOT button, then press RESET)"
    print_warning "Press Enter to continue or Ctrl+C to cancel..."
    read -r

    echo ""
    print_info "Starting flash process..."
    echo ""

    # Flash using esptool.py
    if ! $ESPTOOL --chip "$CHIP" --port "$port" --baud "$baud" write_flash "$FLASH_OFFSET" "$firmware_bin"; then
        print_error "Flash failed!"
        exit 1
    fi

    echo ""
    print_success "Flash completed successfully!"
    print_info "You can now reset the ESP32-S3 (press RESET button)"
}

verify_chip() {
    local port="$1"

    print_info "Verifying ESP32-S3 connection..."

    if ! $ESPTOOL --chip "$CHIP" --port "$port" chip_id &> /dev/null; then
        print_error "Failed to communicate with ESP32-S3"
        print_info "Make sure:"
        print_info "  1. ESP32-S3 is connected to $port"
        print_info "  2. USB cable is connected"
        print_info "  3. ESP32-S3 is in download mode (hold BOOT, press RESET)"
        return 1
    fi

    print_success "ESP32-S3 detected"
    return 0
}

main() {
    # Check for help flag
    if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
        usage
    fi

    local version="${1:-latest}"
    local port="${2:-}"
    local baud="${3:-$DEFAULT_BAUD}"

    print_info "Flash Firmware Tool v${SCRIPT_VERSION}"
    print_info "Repository: ${GITHUB_REPO}"
    print_info "Firmware version: $version"
    print_info "Target chip: $CHIP"
    echo ""

    check_dependencies

    # Get release information
    local release_info
    if [ "$version" = "latest" ]; then
        release_info=$(get_latest_release)
    else
        release_info=$(get_release_by_tag "$version")
    fi

    local tag_name=$(echo "$release_info" | jq -r '.tag_name')
    local release_name=$(echo "$release_info" | jq -r '.name')

    print_info "Found release: $release_name ($tag_name)"

    # Download and extract firmware
    local firmware_bin=$(download_firmware "$release_info")

    if [ ! -f "$firmware_bin" ]; then
        print_error "Firmware file does not exist: $firmware_bin"
        exit 1
    fi

    # Detect or use specified port
    if [ -z "$port" ]; then
        port=$(detect_port)
    fi

    # Verify chip connection (optional, non-fatal)
    verify_chip "$port" || true

    # Flash firmware
    flash_firmware "$firmware_bin" "$port" "$baud"

    print_success "Firmware flash completed successfully!"
    print_info "Next steps:"
    print_info "  1. Press RESET button on ESP32-S3"
    print_info "  2. Connect to WiFi provisioning AP (serial-XXXXXX)"
    print_info "  3. Configure WiFi credentials"
    print_info "  4. Access Web UI at http://serial-XXXXXX.local/"
    exit 0
}

main "$@"
