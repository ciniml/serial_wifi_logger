#!/bin/bash
#
# gen_keys.sh - WireGuard テスト用鍵ペア生成スクリプト
#
# Linux ホスト (peer) と ESP32 (client) の両方の鍵ペアを生成し、
# setup_esp32_nvs.sh に渡せる形式で出力します。
#
# 必要なパッケージ: wireguard-tools
#   Ubuntu/Debian: sudo apt-get install wireguard-tools
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${1:-${SCRIPT_DIR}/keys}"

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()    { echo -e "${BLUE}[INFO]${NC} $*"; }
success() { echo -e "${GREEN}[OK]${NC}   $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; }
error()   { echo -e "${RED}[ERR]${NC}  $*" >&2; }

if ! command -v wg &>/dev/null; then
    error "wg コマンドが見つかりません。wireguard-tools をインストールしてください。"
    error "  Ubuntu/Debian: sudo apt-get install wireguard-tools"
    exit 1
fi

if [[ -d "$OUTPUT_DIR" ]]; then
    warn "出力ディレクトリが既に存在します: $OUTPUT_DIR"
    read -rp "上書きしますか？ [y/N] " ans
    [[ "$ans" =~ ^[Yy]$ ]] || { info "中止しました。"; exit 0; }
fi

mkdir -p "$OUTPUT_DIR"
chmod 700 "$OUTPUT_DIR"

info "Linux ホスト (peer) の鍵ペアを生成..."
SERVER_PRIV=$(wg genkey)
SERVER_PUB=$(echo "$SERVER_PRIV" | wg pubkey)
echo "$SERVER_PRIV" > "$OUTPUT_DIR/server_private.key"
echo "$SERVER_PUB"  > "$OUTPUT_DIR/server_public.key"
chmod 600 "$OUTPUT_DIR/server_private.key"
success "サーバー秘密鍵: $OUTPUT_DIR/server_private.key"
success "サーバー公開鍵: $SERVER_PUB"

info "ESP32 (client) の鍵ペアを生成..."
CLIENT_PRIV=$(wg genkey)
CLIENT_PUB=$(echo "$CLIENT_PRIV" | wg pubkey)
echo "$CLIENT_PRIV" > "$OUTPUT_DIR/client_private.key"
echo "$CLIENT_PUB"  > "$OUTPUT_DIR/client_public.key"
chmod 600 "$OUTPUT_DIR/client_private.key"
success "クライアント秘密鍵: $OUTPUT_DIR/client_private.key"
success "クライアント公開鍵: $CLIENT_PUB"

# setup_esp32_nvs.sh 用の設定ファイルを生成
cat > "$OUTPUT_DIR/wg_params.env" << EOF
# WireGuard テスト用パラメータ
# このファイルを setup_esp32_nvs.sh および start_server.sh が読み込みます

# Linux ホスト (peer) 側
SERVER_PRIVATE_KEY="${SERVER_PRIV}"
SERVER_PUBLIC_KEY="${SERVER_PUB}"
SERVER_WG_IP="10.0.0.1"
SERVER_WG_PORT="51820"

# ESP32 (client) 側
CLIENT_PRIVATE_KEY="${CLIENT_PRIV}"
CLIENT_PUBLIC_KEY="${CLIENT_PUB}"
CLIENT_WG_IP="10.0.0.2"
CLIENT_WG_NETMASK="255.255.255.0"
EOF
chmod 600 "$OUTPUT_DIR/wg_params.env"

echo ""
success "鍵の生成が完了しました: $OUTPUT_DIR/"
info "次のステップ:"
info "  1. Linux ホストの IP アドレスを確認してください (例: ip addr show)"
info "  2. ./start_server.sh <LAN_IP> でピアを起動"
info "  3. ./setup_esp32_nvs.sh <LAN_IP> [SERIAL_PORT] で ESP32 に書き込み"
