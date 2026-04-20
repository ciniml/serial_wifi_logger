# WireGuard テスト手順

ローカルネットワーク上の Linux ホストを WireGuard ピアとして使い、ESP32-S3 との VPN トンネルを動作確認する手順です。

## 構成図

```
[ESP32-S3]─────WiFi─────[Wi-Fi ルーター]─────LAN─────[Linux PC]
  WG IP: 10.0.0.2                                      WG IP: 10.0.0.1
  LAN: DHCP 取得                                        LAN: 192.168.x.x (固定 or DHCP)

          <────────── WireGuard トンネル (UDP) ──────────>
```

ESP32 と Linux PC が**同じ LAN に繋がっていれば**、インターネット接続は不要です。

---

## 必要なパッケージ (Linux)

```bash
sudo apt-get install wireguard-tools
```

esptool と ESP-IDF もセットアップ済みであること:

```bash
source ~/esp-idf/6.0/export.sh
```

---

## 手順

### 1. 鍵ペアの生成

```bash
cd script/wireguard
./gen_keys.sh
```

`keys/` ディレクトリに以下が生成されます:

| ファイル | 内容 |
|---|---|
| `keys/server_private.key` | Linux ホスト (ピア) の秘密鍵 |
| `keys/server_public.key`  | Linux ホスト (ピア) の公開鍵 |
| `keys/client_private.key` | ESP32 の秘密鍵 |
| `keys/client_public.key`  | ESP32 の公開鍵 |
| `keys/wg_params.env`      | スクリプト間で共有するパラメータ |

> **注意:** `keys/` ディレクトリには秘密鍵が含まれます。リポジトリにコミットしないでください (`keys/` は `.gitignore` で除外済みです)。

---

### 2. Linux ホストで WireGuard ピアを起動

Linux ホストの LAN 側 IP を確認します:

```bash
ip addr show | grep "inet " | grep -v 127.0.0.1
# 例: 192.168.1.10
```

ピアを起動します:

```bash
./start_server.sh 192.168.1.10
```

起動後、`sudo wg show wg_test` でインターフェースが上がっていることを確認してください。

停止する場合:

```bash
./start_server.sh stop
```

---

### 3. ESP32 に WireGuard 設定を書き込む

ESP32 をダウンロードモードにして (BOOT ボタンを押しながら RESET) シリアル接続し、NVS に設定を書き込みます:

```bash
./setup_esp32_nvs.sh 192.168.1.10
# ポートを明示する場合:
./setup_esp32_nvs.sh 192.168.1.10 /dev/ttyUSB0
```

スクリプトは以下の NVS キー (namespace: `wireguard`) を書き込みます:

| NVS キー | 内容 | 値 |
|---|---|---|
| `private_key`   | ESP32 の WireGuard 秘密鍵 (base64) | gen_keys.sh で生成 |
| `peer_pub_key`  | Linux ホストの公開鍵 (base64) | gen_keys.sh で生成 |
| `peer_endpoint` | Linux ホストの LAN IP | 引数で指定 |
| `peer_port`     | WireGuard UDP ポート | 51820 |
| `listen_port`   | ESP32 側 UDP ポート | 51820 |
| `keepalive`     | キープアライブ間隔 (秒) | 25 |
| `local_ip`      | ESP32 の WireGuard IP | 10.0.0.2 |
| `local_netmask` | サブネットマスク | 255.255.255.0 |
| `set_default`   | デフォルトルート設定 | 0 (無効) |
| `ntp_server`    | NTP サーバー | ntp1.mfeed.ad.jp |

> `set_default=0` にすることで、LAN 側の TCP/RFC2217 ポートへのアクセスを維持しながら WireGuard トンネルも使えます。

---

### 4. ESP32 をリブートして接続を待つ

NVS 書き込み後、RESET ボタンを押します。ESP32 は以下の順で動作します:

1. WiFi に接続
2. NTP で時刻同期 (`ntp1.mfeed.ad.jp`)
3. WireGuard トンネルを確立

シリアルモニタで確認する場合:

```bash
source ~/esp-idf/6.0/export.sh
idf.py -p /dev/ttyUSB0 monitor
```

正常動作時のログ例:

```
I (1234) wireguard: Synchronizing time via NTP server: ntp1.mfeed.ad.jp (timeout 10000 ms)
I (2345) wireguard: NTP sync OK: unix time 1745123456
I (2346) wireguard: Starting WireGuard VPN...
I (2500) wireguard: Peer endpoint 192.168.1.10 -> 192.168.1.10
I (2600) wireguard: WireGuard tunnel established
```

---

### 5. 疎通確認

```bash
./test_connection.sh
```

以下の項目を自動確認します:

| チェック項目 | 確認内容 |
|---|---|
| インターフェース存在 | `wg_test` が起動しているか |
| ハンドシェイク | 最後のハンドシェイクが 3 分以内か |
| ping | `10.0.0.1` → `10.0.0.2` に ICMP が通るか |
| トラフィック | 双方向のバイトカウントが 0 でないか |
| TCP ポート | WireGuard IP 経由で RFC2217 (2217) に接続できるか |

---

## WireGuard 経由での RFC2217 接続

トンネル確立後、通常のシリアルアクセスが WireGuard IP 経由でも可能です:

```bash
# pyserial を使った接続例
python3 -m serial.tools.miniterm rfc2217://10.0.0.2:2217
```

---

## トラブルシューティング

### ハンドシェイクが確立しない

- ESP32 のシリアルログで NTP 同期が成功しているか確認 (`NTP sync OK`)
- NTP 同期が失敗している場合、Wi-Fi が繋がっているか確認
- 鍵の設定ミス: `gen_keys.sh` を再実行して `setup_esp32_nvs.sh` でリセット

### ping が通らない

```bash
# ファイアウォールの確認
sudo ufw status
sudo ufw allow 51820/udp

# iptables の場合
sudo iptables -A INPUT -p udp --dport 51820 -j ACCEPT
```

### ハンドシェイクはあるが ping が通らない

`set_default=0` の場合、Linux 側に `10.0.0.2` へのルートが必要です。`wg-quick up` が自動設定しますが、手動確認:

```bash
ip route show | grep 10.0.0
# 10.0.0.0/24 dev wg_test  が表示されれば OK
```

### NVS を出荷状態に戻す

```bash
# NVS 全体を消去 (全設定がリセットされます)
esptool.py --chip esp32s3 --port /dev/ttyUSB0 erase_region 0x9000 0x4000
```

---

## ファイル一覧

| ファイル | 説明 |
|---|---|
| `gen_keys.sh`        | WireGuard 鍵ペアを生成 |
| `start_server.sh`    | Linux ホストで WireGuard ピアを起動・停止 |
| `setup_esp32_nvs.sh` | ESP32 の NVS に WireGuard 設定を書き込む |
| `test_connection.sh` | トンネルの疎通確認を自動実行 |
| `keys/`              | 生成された鍵ファイル (gitignore 対象) |
