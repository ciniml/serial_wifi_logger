#!/bin/bash
#
# setup_esp32_nvs.sh - ESP32 の NVS パーティションに WireGuard 設定を書き込むスクリプト
#
# gen_keys.sh で生成した keys/wg_params.env と指定した LAN IP を使い、
# NVS パーティションイメージを作成して esptool.py でフラッシュします。
#
# 使用方法:
#   ./setup_esp32_nvs.sh <SERVER_LAN_IP> [SERIAL_PORT] [KEYS_DIR]
#
#   SERVER_LAN_IP : Linux ホストの LAN 側 IP アドレス
#   SERIAL_PORT   : ESP32 のシリアルポート (省略時: 自動検出)
#   KEYS_DIR      : gen_keys.sh の出力ディレクトリ (省略時: ./keys)
#
# 依存ツール:
#   - Python 3 + ESP-IDF (nvs_partition_gen.py, esptool.py)
#   - source ~/esp-idf/6.0/export.sh でセットアップ済みであること
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# NVS パーティション情報 (partitions.csv に合わせる)
NVS_OFFSET="0x9000"
NVS_SIZE="0x4000"   # 16 KB

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()    { echo -e "${BLUE}[INFO]${NC} $*"; }
success() { echo -e "${GREEN}[OK]${NC}   $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; }
error()   { echo -e "${RED}[ERR]${NC}  $*" >&2; }

usage() {
    cat << EOF
使用方法: $0 <SERVER_LAN_IP> [SERIAL_PORT] [KEYS_DIR]

  SERVER_LAN_IP  Linux ホストの LAN 側 IP (例: 192.168.1.10)
  SERIAL_PORT    ESP32 のシリアルポート (例: /dev/ttyUSB0)  省略時: 自動検出
  KEYS_DIR       gen_keys.sh の出力ディレクトリ             省略時: ./keys

例:
  $0 192.168.1.10
  $0 192.168.1.10 /dev/ttyUSB0
  $0 192.168.1.10 /dev/ttyUSB0 ./keys
EOF
    exit 1
}

detect_port() {
    local ports=()
    for p in /dev/ttyUSB* /dev/ttyACM* /dev/cu.usbserial* /dev/cu.SLAB_USBtoUART*; do
        [[ -e "$p" ]] && ports+=("$p")
    done
    if [[ ${#ports[@]} -eq 0 ]]; then
        error "シリアルポートが見つかりません。ポートを手動で指定してください。"
        exit 1
    fi
    echo "${ports[0]}"
}

[[ "${1:-}" =~ ^(-h|--help)$ ]] && usage
SERVER_LAN_IP="${1:?$(usage)}"
SERIAL_PORT="${2:-}"
KEYS_DIR="${3:-${SCRIPT_DIR}/keys}"
PARAMS_FILE="${KEYS_DIR}/wg_params.env"

if [[ ! -f "$PARAMS_FILE" ]]; then
    error "パラメータファイルが見つかりません: $PARAMS_FILE"
    error "先に gen_keys.sh を実行してください。"
    exit 1
fi

# shellcheck source=/dev/null
source "$PARAMS_FILE"

# ESP-IDF ツールの確認
if ! command -v python3 &>/dev/null; then
    error "python3 が見つかりません。"
    exit 1
fi

NVS_GEN=""
if python3 -c "import esp_idf_nvs_partition_gen" &>/dev/null 2>&1; then
    NVS_GEN="python3 -m esp_idf_nvs_partition_gen"
elif [[ -n "${IDF_PATH:-}" ]]; then
    CANDIDATE="${IDF_PATH}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    [[ -f "$CANDIDATE" ]] && NVS_GEN="python3 $CANDIDATE"
fi

if [[ -z "$NVS_GEN" ]]; then
    error "nvs_partition_gen.py が見つかりません。"
    error "ESP-IDF をセットアップしてから実行してください:"
    error "  source ~/esp-idf/6.0/export.sh"
    exit 1
fi

ESPTOOL=""
if command -v esptool.py &>/dev/null; then
    ESPTOOL="esptool.py"
elif command -v esptool &>/dev/null; then
    ESPTOOL="esptool"
elif [[ -n "${IDF_PATH:-}" ]] && [[ -f "${IDF_PATH}/components/esptool_py/esptool/esptool.py" ]]; then
    ESPTOOL="python3 ${IDF_PATH}/components/esptool_py/esptool/esptool.py"
fi

if [[ -z "$ESPTOOL" ]]; then
    error "esptool が見つかりません。pip install esptool または ESP-IDF をセットアップしてください。"
    exit 1
fi

[[ -z "$SERIAL_PORT" ]] && SERIAL_PORT=$(detect_port)

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

NVS_CSV="${TMP_DIR}/wireguard_nvs.csv"
NVS_BIN="${TMP_DIR}/wireguard_nvs.bin"

info "NVS CSV を生成: ${NVS_CSV}"

cat > "$NVS_CSV" << EOF
key,type,encoding,value
wireguard,namespace,,
private_key,data,string,${CLIENT_PRIVATE_KEY}
peer_pub_key,data,string,${SERVER_PUBLIC_KEY}
peer_endpoint,data,string,${SERVER_LAN_IP}
peer_port,data,u16,${SERVER_WG_PORT}
listen_port,data,u16,51820
keepalive,data,u16,25
local_ip,data,string,${CLIENT_WG_IP}
local_netmask,data,string,${CLIENT_WG_NETMASK}
set_default,data,u8,0
ntp_server,data,string,ntp1.mfeed.ad.jp
EOF

info "NVS パーティションイメージを生成: ${NVS_BIN}"
$NVS_GEN generate "$NVS_CSV" "$NVS_BIN" "$NVS_SIZE"
success "NVS イメージ生成完了 ($(du -h "$NVS_BIN" | cut -f1))"

echo ""
info "書き込み先:"
info "  ポート   : ${SERIAL_PORT}"
info "  オフセット: ${NVS_OFFSET}"
echo ""
warn "ESP32-S3 がダウンロードモードになっていることを確認してください。"
warn "  (BOOT ボタンを押しながら RESET ボタンを押して離す → BOOT ボタンを離す)"
read -rp "書き込みを開始しますか？ [y/N] " ans
[[ "$ans" =~ ^[Yy]$ ]] || { info "中止しました。"; exit 0; }

info "NVS パーティションを書き込み中..."
$ESPTOOL --chip esp32s3 --port "$SERIAL_PORT" --baud 460800 \
    write_flash "$NVS_OFFSET" "$NVS_BIN"

echo ""
success "NVS 書き込みが完了しました！"
info "次のステップ:"
info "  1. ESP32 の RESET ボタンを押してリブート"
info "  2. Linux ホストで WireGuard ピアを起動 (start_server.sh)"
info "  3. test_connection.sh でトンネルの疎通確認"
