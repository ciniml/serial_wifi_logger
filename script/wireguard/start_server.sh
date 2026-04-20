#!/bin/bash
#
# start_server.sh - Linux ホスト上で WireGuard ピアを起動するスクリプト
#
# gen_keys.sh で生成した keys/wg_params.env を読み込み、
# /etc/wireguard/wg_test.conf を作成して wg-quick で起動します。
#
# 使用方法:
#   ./start_server.sh <LAN_IP> [KEYS_DIR]
#
#   LAN_IP   : このホストの LAN 側 IP アドレス (ESP32 が接続先として使う)
#   KEYS_DIR : gen_keys.sh の出力ディレクトリ (省略時: ./keys)
#
# 停止:
#   ./start_server.sh stop
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WG_IFACE="wg_test"
WG_CONF="/etc/wireguard/${WG_IFACE}.conf"

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()    { echo -e "${BLUE}[INFO]${NC} $*"; }
success() { echo -e "${GREEN}[OK]${NC}   $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; }
error()   { echo -e "${RED}[ERR]${NC}  $*" >&2; }

if [[ "${1:-}" == "stop" ]]; then
    info "WireGuard インターフェース ${WG_IFACE} を停止します..."
    sudo wg-quick down "$WG_IFACE" && success "停止しました。" || warn "すでに停止しているかもしれません。"
    exit 0
fi

LAN_IP="${1:?使用方法: $0 <LAN_IP> [KEYS_DIR]}"
KEYS_DIR="${2:-${SCRIPT_DIR}/keys}"
PARAMS_FILE="${KEYS_DIR}/wg_params.env"

if [[ ! -f "$PARAMS_FILE" ]]; then
    error "パラメータファイルが見つかりません: $PARAMS_FILE"
    error "先に gen_keys.sh を実行してください。"
    exit 1
fi

# shellcheck source=/dev/null
source "$PARAMS_FILE"

if ! command -v wg-quick &>/dev/null; then
    error "wg-quick が見つかりません。wireguard-tools をインストールしてください。"
    error "  Ubuntu/Debian: sudo apt-get install wireguard-tools"
    exit 1
fi

info "WireGuard 設定ファイルを生成: ${WG_CONF}"

sudo tee "$WG_CONF" > /dev/null << EOF
# WireGuard テスト用設定 (自動生成)
# start_server.sh により作成

[Interface]
PrivateKey = ${SERVER_PRIVATE_KEY}
Address    = ${SERVER_WG_IP}/24
ListenPort = ${SERVER_WG_PORT}

# ESP32 クライアント
[Peer]
PublicKey  = ${CLIENT_PUBLIC_KEY}
AllowedIPs = ${CLIENT_WG_IP}/32
EOF

sudo chmod 600 "$WG_CONF"
success "設定ファイルを作成しました。"

# ファイアウォール設定の確認・案内
if command -v ufw &>/dev/null; then
    if sudo ufw status | grep -q "Status: active"; then
        info "UFW が有効です。ポート ${SERVER_WG_PORT}/udp を開放します..."
        sudo ufw allow "${SERVER_WG_PORT}/udp" comment "WireGuard test"
        success "UFW ルールを追加しました。"
    fi
fi

# すでに起動中なら再起動
if sudo wg show "$WG_IFACE" &>/dev/null 2>&1; then
    info "インターフェース ${WG_IFACE} を再起動します..."
    sudo wg-quick down "$WG_IFACE" || true
fi

info "WireGuard インターフェース ${WG_IFACE} を起動します..."
sudo wg-quick up "$WG_IFACE"

echo ""
success "WireGuard ピアが起動しました。"
info "  インターフェース : ${WG_IFACE}"
info "  サーバー WG IP   : ${SERVER_WG_IP}/24"
info "  LAN IP           : ${LAN_IP}"
info "  待受ポート        : ${SERVER_WG_PORT}/udp"
info "  許可ピア          : ESP32 (${CLIENT_WG_IP})"
echo ""
info "ESP32 に書き込む際はこの LAN IP を使用してください:"
info "  ./setup_esp32_nvs.sh ${LAN_IP}"
echo ""
info "ピアの状態確認:"
info "  sudo wg show ${WG_IFACE}"
echo ""
info "停止する場合:"
info "  ./start_server.sh stop"
