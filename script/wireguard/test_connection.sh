#!/bin/bash
#
# test_connection.sh - WireGuard トンネルの疎通確認スクリプト
#
# ESP32 との WireGuard トンネルが正常に確立されているかを確認します。
#
# 使用方法:
#   ./test_connection.sh [KEYS_DIR]
#
#   KEYS_DIR : gen_keys.sh の出力ディレクトリ (省略時: ./keys)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KEYS_DIR="${1:-${SCRIPT_DIR}/keys}"
PARAMS_FILE="${KEYS_DIR}/wg_params.env"
WG_IFACE="wg_test"

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()    { echo -e "${BLUE}[INFO]${NC} $*"; }
success() { echo -e "${GREEN}[PASS]${NC} $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; }
fail()    { echo -e "${RED}[FAIL]${NC} $*"; }

PASS=0
FAIL=0

check() {
    local desc="$1"
    shift
    if "$@" &>/dev/null; then
        success "$desc"
        ((PASS++)) || true
    else
        fail "$desc"
        ((FAIL++)) || true
    fi
}

if [[ ! -f "$PARAMS_FILE" ]]; then
    echo "パラメータファイルが見つかりません: $PARAMS_FILE"
    echo "先に gen_keys.sh を実行してください。"
    exit 1
fi

# shellcheck source=/dev/null
source "$PARAMS_FILE"

echo "========================================"
echo " WireGuard 接続テスト"
echo "========================================"
echo ""

# 1. インターフェースの存在確認
info "[1/6] WireGuard インターフェースの確認..."
check "インターフェース ${WG_IFACE} が存在する" sudo wg show "$WG_IFACE"

# 2. ハンドシェイク確認
info "[2/6] ハンドシェイクの確認..."
LAST_HS=$(sudo wg show "$WG_IFACE" latest-handshakes 2>/dev/null | awk '{print $2}' | head -1)
NOW=$(date +%s)
if [[ -n "$LAST_HS" && "$LAST_HS" != "0" ]]; then
    ELAPSED=$(( NOW - LAST_HS ))
    if (( ELAPSED < 180 )); then
        success "ハンドシェイク成立 (${ELAPSED}秒前)"
        ((PASS++)) || true
    else
        warn "ハンドシェイクが古い (${ELAPSED}秒前) — ESP32 がまだ接続中かもしれません"
        ((FAIL++)) || true
    fi
else
    fail "ハンドシェイクが未確立"
    ((FAIL++)) || true
fi

# 3. ping (サーバー→ESP32)
info "[3/6] ping: ${SERVER_WG_IP} → ${CLIENT_WG_IP} ..."
if ping -c 3 -W 5 "${CLIENT_WG_IP}" &>/dev/null; then
    RTT=$(ping -c 3 -W 5 "${CLIENT_WG_IP}" 2>/dev/null | tail -1 | grep -oP 'avg = \K[0-9.]+' || \
          ping -c 3 -W 5 "${CLIENT_WG_IP}" 2>/dev/null | tail -1 | awk -F'/' '{print $5}')
    success "ping 応答あり (avg RTT: ${RTT:-?} ms)"
    ((PASS++)) || true
else
    fail "ping 応答なし (${CLIENT_WG_IP})"
    ((FAIL++)) || true
fi

# 4. トラフィックカウンタ確認
info "[4/6] トラフィックカウンタ確認..."
RX_BYTES=$(sudo wg show "$WG_IFACE" transfer 2>/dev/null | awk '{print $2}' | head -1)
TX_BYTES=$(sudo wg show "$WG_IFACE" transfer 2>/dev/null | awk '{print $3}' | head -1)
if [[ -n "$RX_BYTES" && "$RX_BYTES" != "0" ]]; then
    success "双方向トラフィックあり (RX: ${RX_BYTES}B, TX: ${TX_BYTES}B)"
    ((PASS++)) || true
else
    fail "トラフィックがゼロ — トンネルが機能していない可能性があります"
    ((FAIL++)) || true
fi

# 5. TCP ポート到達確認 (WireGuard IP 経由で RFC2217 ポートに接続)
info "[5/6] TCP ポート確認: ${CLIENT_WG_IP}:2217 (RFC2217)..."
if command -v nc &>/dev/null; then
    if nc -z -w 5 "${CLIENT_WG_IP}" 2217 &>/dev/null; then
        success "RFC2217 ポート (2217) に到達"
        ((PASS++)) || true
    else
        warn "RFC2217 ポート (2217) に到達できません (USB デバイス未接続の可能性あり)"
        ((FAIL++)) || true
    fi
else
    warn "nc コマンドがないため TCP ポートテストをスキップ"
fi

# 6. wg show の詳細表示
info "[6/6] WireGuard インターフェース詳細:"
echo "----------------------------------------"
sudo wg show "$WG_IFACE" 2>/dev/null || true
echo "----------------------------------------"

echo ""
echo "========================================"
echo " テスト結果: PASS=${PASS}  FAIL=${FAIL}"
echo "========================================"

if [[ $FAIL -eq 0 ]]; then
    success "全テスト通過！WireGuard トンネルは正常に動作しています。"
    exit 0
else
    fail "${FAIL} 件のテストが失敗しました。"
    echo ""
    info "トラブルシューティング:"
    info "  - ESP32 のシリアルログを確認: idf.py -p <PORT> monitor"
    info "  - ハンドシェイク未確立の場合: NTP 同期時刻・鍵の設定を確認"
    info "  - ping 失敗の場合: ファイアウォール (ufw/iptables) を確認"
    info "  - ファイアウォール: sudo ufw allow 51820/udp"
    exit 1
fi
