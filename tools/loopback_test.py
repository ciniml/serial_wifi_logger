#!/usr/bin/env python3
"""
RFC2217 loopback integrity test.

Hardware setup: short TX and RX pins on the USB-UART device connected to the ESP32.

Usage:
    python3 loopback_test.py <ip> [--port 2217] [--baud 115200] [--count 4]

Example:
    python3 loopback_test.py 192.168.1.100
    python3 loopback_test.py 192.168.1.100 --baud 9600 --count 8
"""

import argparse
import sys
import serial


def run_loopback_test(url: str, baud: int, count: int) -> bool:
    # Build test payload first so we can compute the read timeout.
    # All 256 byte values repeated `count` times.
    # Including 0xFF (IAC) verifies that IAC escaping works correctly.
    tx_data = bytes(range(256)) * count

    # Timeout: time to transmit the full payload at the given baud rate (10 bits/byte),
    # multiplied by 3 for loopback overhead, plus 5 s fixed margin.
    read_timeout = max(10.0, len(tx_data) * 10 / baud * 3 + 5)

    print(f"Connecting to {url} at {baud} baud ...")

    try:
        port = serial.serial_for_url(url, baudrate=baud, timeout=read_timeout)
    except Exception as e:
        print(f"ERROR: Failed to open port: {e}", file=sys.stderr)
        return False

    with port:
        # Flush any stale data
        port.reset_input_buffer()
        port.reset_output_buffer()

        print(f"Sending {len(tx_data)} bytes (read timeout {read_timeout:.1f}s) ...")

        # Send in small chunks so the ESP32's lwIP receive buffer never saturates.
        # At 115200 baud the USB serial drains ~11.5 KB/s; 1 KB chunks keep the
        # pipeline full without overwhelming the TCP receive window.
        chunk_size = 1024
        for offset in range(0, len(tx_data), chunk_size):
            port.write(tx_data[offset:offset + chunk_size])
        port.flush()

        rx_data = port.read(len(tx_data))

    if len(rx_data) != len(tx_data):
        print(f"FAIL: expected {len(tx_data)} bytes, received {len(rx_data)} bytes")
        return False

    first_mismatch = next(
        (i for i, (t, r) in enumerate(zip(tx_data, rx_data)) if t != r), None
    )
    if first_mismatch is not None:
        print(
            f"FAIL: first mismatch at byte {first_mismatch}: "
            f"expected 0x{tx_data[first_mismatch]:02X}, "
            f"got 0x{rx_data[first_mismatch]:02X}"
        )
        return False

    print(f"PASS: {len(tx_data)} bytes matched exactly")
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description="RFC2217 loopback integrity test")
    parser.add_argument("ip", help="ESP32 IP address")
    parser.add_argument("--port", type=int, default=2217, help="RFC2217 port (default: 2217)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument(
        "--count",
        type=int,
        default=4,
        help="Number of times to repeat the 256-byte pattern (default: 4 = 1024 bytes)",
    )
    args = parser.parse_args()

    url = f"rfc2217://{args.ip}:{args.port}"
    ok = run_loopback_test(url, args.baud, args.count)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
