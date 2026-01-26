#!/bin/bash
#
# OTA Firmware Update Script
# Downloads firmware from GitHub releases and uploads to device
#
# Usage:
#   ./ota_update.sh <device> [version]
#   ./ota_update.sh serial-A02048.local v1.0.0
#   ./ota_update.sh 192.168.1.100 latest
#   ./ota_update.sh serial-A02048.local  # Uses latest version
#

set -e

# Enable debug mode with DEBUG=1 environment variable
if [ "${DEBUG:-0}" = "1" ]; then
    set -x
fi

# Script version
SCRIPT_VERSION="1.0.2"

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
TMP_DIR="/tmp/ota_update_$$"

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
Usage: $0 <device> [version]

Arguments:
  device    Device hostname (e.g., serial-A02048.local) or IP address
  version   GitHub release version (e.g., v1.0.0) or "latest" (default: latest)

Examples:
  $0 serial-A02048.local v1.0.0
  $0 192.168.1.100 latest
  $0 serial-A02048.local

Environment Variables:
  GITHUB_TOKEN   Optional GitHub personal access token for private repos
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

    local firmware_bin="$TMP_DIR/serial_wifi_logger.bin"

    if [ ! -f "$firmware_bin" ]; then
        print_error "Firmware binary not found in ZIP: serial_wifi_logger.bin" >&2
        print_info "Available files in ZIP:" >&2
        unzip -l "$zip_file" >&2
        exit 1
    fi

    print_success "Extracted firmware: $(du -h "$firmware_bin" | cut -f1)" >&2
    echo "$firmware_bin"
}

get_device_info() {
    local device="$1"
    local url="http://$device/api/info"

    print_info "Fetching device information from $url"

    local response=$(curl -s --max-time 5 "$url" 2>/dev/null || echo "")

    if [ -z "$response" ]; then
        print_warning "Failed to connect to device at $device"
        print_warning "Make sure the device is online and accessible"
        return 1
    fi

    if ! echo "$response" | jq -e . &> /dev/null; then
        print_warning "Invalid JSON response from device"
        return 1
    fi

    local version=$(echo "$response" | jq -r '.version')
    local partition=$(echo "$response" | jq -r '.partition')
    local uptime=$(echo "$response" | jq -r '.uptime')

    echo ""
    echo "════════════════════════════════════════"
    echo "  Device Information"
    echo "════════════════════════════════════════"
    echo "  Address:    $device"
    echo "  Version:    $version"
    echo "  Partition:  $partition"
    echo "  Uptime:     ${uptime}s"
    echo "════════════════════════════════════════"
    echo ""

    return 0
}

upload_firmware() {
    local device="$1"
    local firmware_bin="$2"
    local url="http://$device/api/ota"

    print_info "Uploading firmware to $device..."
    print_info "This may take 30-60 seconds depending on WiFi speed"

    # Use a temporary file to capture response
    local temp_response="/tmp/ota_response_$$"

    local http_code=$(curl -X POST \
        --data-binary "@$firmware_bin" \
        -H "Content-Type: application/octet-stream" \
        -w "%{http_code}" \
        --max-time 120 \
        --progress-bar \
        -o "$temp_response" \
        "$url" 2>&2)

    local body=$(cat "$temp_response" 2>/dev/null || echo "")
    rm -f "$temp_response"

    echo ""

    if [ "$http_code" = "200" ] && [ "$body" = "OK" ]; then
        print_success "Firmware uploaded successfully!"
        print_info "Device will reboot in 3 seconds..."
        print_info "Waiting for device to restart..."
        sleep 10
        return 0
    else
        print_error "Upload failed!"
        print_error "HTTP Status: $http_code"
        print_error "Response: $body"
        return 1
    fi
}

verify_update() {
    local device="$1"
    local expected_version="$2"

    print_info "Verifying firmware update..."
    print_info "Waiting for device to fully boot (15 seconds)..."
    sleep 15

    local response=$(curl -s --max-time 5 "http://$device/api/info" 2>/dev/null || echo "")

    if [ -z "$response" ]; then
        print_warning "Device not responding yet. It may need more time to boot."
        return 1
    fi

    local new_version=$(echo "$response" | jq -r '.version' 2>/dev/null || echo "")
    local new_partition=$(echo "$response" | jq -r '.partition' 2>/dev/null || echo "")

    echo ""
    echo "════════════════════════════════════════"
    echo "  Updated Device Information"
    echo "════════════════════════════════════════"
    echo "  Version:    $new_version"
    echo "  Partition:  $new_partition"
    echo "════════════════════════════════════════"
    echo ""

    if [ -n "$new_version" ] && [ "$new_version" != "null" ]; then
        print_success "Device is running with new firmware!"
        return 0
    else
        print_warning "Could not verify new version"
        return 1
    fi
}

main() {
    # Check for help flag
    if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
        usage
    fi

    local device="$1"
    local version="${2:-latest}"

    print_info "OTA Firmware Update Tool v${SCRIPT_VERSION}"
    print_info "Repository: ${GITHUB_REPO}"
    print_info "Target device: $device"
    print_info "Firmware version: $version"
    echo ""

    check_dependencies

    # Get device info before update
    if get_device_info "$device"; then
        read -p "Continue with OTA update? (y/N): " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            print_info "Update cancelled"
            exit 0
        fi
    else
        read -p "Device not responding. Continue anyway? (y/N): " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            print_info "Update cancelled"
            exit 0
        fi
    fi

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

    # Upload to device
    if upload_firmware "$device" "$firmware_bin"; then
        # Verify update
        if verify_update "$device" "$tag_name"; then
            print_success "OTA update completed successfully!"
            print_info "Web UI: http://$device/"
            exit 0
        else
            print_warning "Update may have succeeded, but verification failed"
            print_info "Check device manually: http://$device/"
            exit 0
        fi
    else
        print_error "OTA update failed"
        exit 1
    fi
}

main "$@"
