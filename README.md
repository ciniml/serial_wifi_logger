# Serial WiFi Logger

| 対応ターゲット | ESP32-S3 |
| ------------- | -------- |

## 概要

Serial WiFi Loggerは、ESP32-S3のUSB OTG機能を使用してUSBシリアルデバイスをネットワーク経由で利用可能にするファームウェアです。USB CDC-ACMデバイスとFTDIデバイスの両方に対応し、TCPソケット経由でシリアル通信を提供します。

## 主な機能

### 1. USB シリアルホスト機能
- **CDC-ACM対応**: 標準USB通信デバイスクラスのシリアルデバイスに対応
- **FTDI対応**: FTDI製USB-シリアル変換チップ (FT232R, FT2232など) に対応
- **自動検出**: USB VID/PIDに基づいてドライバを自動選択
- **データ転送**: USB → TCP、TCP → USB の双方向データブリッジ

### 2. WiFi ネットワークプロビジョニング
- **SoftAP方式**: 初回起動時にアクセスポイントを立ち上げ
- **自動接続**: 設定後は保存されたWiFi情報で自動接続
- **設定保存**: WiFi接続情報はNVSに永続的に保存

### 3. TCP サーバー機能
- **データポート**: 8888番 (変更可能)
- **制御ポート**: 8889番 (変更可能)
- **接続管理**: 各ポートで1クライアント接続をサポート
- **双方向通信**: USB ↔ TCP 間でリアルタイムデータ転送
- **シリアルポート制御**: DTR/RTS信号、ボーレート設定を制御ポート経由で制御可能

### 4. mDNS サービスディスカバリ
- **自動アドバタイズ**: ネットワーク上でデバイスを自動検出可能
- **サービス名**: `serial-XXXXXX._serial._tcp.local` (XXXXXXはMACアドレスの下位3バイト)
- **動的TXTレコード**: USB接続状態、TCP接続状態、デバイス情報をリアルタイム更新

## 必要なハードウェア

- **ESP32-S3 開発ボード** (USB OTG対応)
- **USB ケーブル** (プログラミング・モニタリング用)
- **USB シリアルデバイス**:
  - CDC-ACMデバイス (別のESP32-S3、Arduinoなど)
  - FTDIデバイス (FT232R USB-シリアル変換アダプタなど)

### ピン配置

ESP32-S3は内部USB OTGピンを使用:
- **GPIO 19**: USB D- (データ線負極)
- **GPIO 20**: USB D+ (データ線正極)

外部ピン設定は不要です。

## ビルドとフラッシュ

### 1. プロジェクトのビルド

```bash
idf.py build
```

### 2. ESP32-S3へのフラッシュ

```bash
idf.py -p PORT flash monitor
```

`PORT`をシリアルポートに置き換えてください (例: Linux では `/dev/ttyUSB0`, Windows では `COM3`)。

シリアルモニタを終了するには `Ctrl-]` を入力してください。

## 初回セットアップ

### WiFi プロビジョニング

1. ファームウェア書き込み後、デバイスはSoftAPモードで起動します
2. SSID `PROV_XXXXXX` のWi-Fiアクセスポイントが表示されます
3. スマートフォンまたはPCでこのアクセスポイントに接続
4. ブラウザで `192.168.4.1` にアクセス
5. 接続先のWi-Fi SSID とパスワードを入力
6. デバイスが設定されたWi-Fiに接続し、以降は自動的に接続されます

プロビジョニング情報をリセットするには:
```bash
idf.py erase-flash
```

## 使用方法

### 1. デバイスの検出

WiFi接続後、mDNSでデバイスを検出できます:

**macOS/Linux:**
```bash
# サービス一覧を表示
dns-sd -B _serial._tcp

# 詳細情報を表示
dns-sd -L serial-XXXXXX _serial._tcp
```

または

```bash
avahi-browse -r _serial._tcp
```

```
$ avahi-browse -rt _serial._tcp
+ enp5s0 IPv4 serial-A02048                                 _serial._tcp         local
= enp5s0 IPv4 serial-A02048                                 _serial._tcp         local
   hostname = [serial-A02048.local]
   address = [192.168.2.125]
   port = [8888]
   txt = ["mac=f4:12:fa:a0:20:48" "ip=192.168.2.125" "port=8888" "usb_connected=1" "tcp_connected=0" "usb_vid=0x0403" "usb_pid=0x6015" "usb_type=FTDI"]
```

**Windows:**
- Bonjour Browserなどのツールを使用

### 2. データポートへの接続

検出したIPアドレスとポート8888に接続してデータ通信:

```bash
# telnet で接続
telnet <IP_ADDRESS> 8888

# nc (netcat) で接続
nc <IP_ADDRESS> 8888

# Python での接続例
python3 -c "
import socket
s = socket.socket()
s.connect(('IP_ADDRESS', 8888))
s.send(b'Hello\\n')
print(s.recv(1024))
"
```

### 3. 制御ポートでのシリアルポート制御

制御ポート（8889番）に接続してDTR/RTS信号やボーレートを制御:

```bash
# 制御ポートに接続
telnet <IP_ADDRESS> 8889

# DTRを1に設定（HIGH）
DTR 1
# 応答: OK

# DTRを0に設定（LOW）
DTR 0
# 応答: OK

# RTSを1に設定（HIGH）
RTS 1
# 応答: OK

# RTSを0に設定（LOW）
RTS 0
# 応答: OK

# ボーレートを9600bpsに設定
BAUD 9600
# 応答: OK

# ボーレートを115200bpsに設定
BAUD 115200
# 応答: OK
```

**対応コマンド:**
- `DTR 0` / `DTR 1` - DTR信号の制御
- `RTS 0` / `RTS 1` - RTS信号の制御
- `BAUD <baudrate>` - ボーレート設定（300～921600bps）

**応答:**
- `OK` - コマンド成功
- `ERROR` - コマンド失敗（USBデバイス未接続、無効なコマンドなど）

### 4. USBシリアルデバイスの接続

1. USBシリアルデバイスをESP32-S3のUSBポートに接続
2. デバイスが自動的に検出され、ドライバが選択されます
3. TCP接続が確立されていれば、即座にデータ通信が可能になります

## mDNS TXTレコード

以下の情報がmDNS TXTレコードとして公開されます:

| キー | 説明 | 例 |
|-----|------|---|
| `mac` | デバイスMACアドレス | `AA:BB:CC:DD:EE:FF` |
| `ip` | デバイスIPアドレス | `192.168.1.100` |
| `port` | TCPデータポート番号 | `8888` |
| `control_port` | TCP制御ポート番号 | `8889` |
| `usb_connected` | USB接続状態 | `0` / `1` |
| `usb_vid` | USB Vendor ID (接続時のみ) | `0x0403` |
| `usb_pid` | USB Product ID (接続時のみ) | `0x6001` |
| `usb_type` | USBドライバタイプ (接続時のみ) | `CDC` / `FTDI` |
| `tcp_connected` | TCPクライアント接続状態 | `0` / `1` |

## 対応デバイス

### CDC-ACMデバイス
- ESP32-S3 (TinyUSB CDC)
- CDC-ACMプロトコルを実装したUSBデバイス全般

### FTDIデバイス
- FT232R (シングルポート)
- FT2232H (デュアルポート)
- FT4232H (クアッドポート)
- FT232H (高速シングルポート)
- VID `0x0403` を持つ他のFTDIチップ

## 設定

### TCP ポート番号の変更

`idf.py menuconfig` → `TCP Server Configuration`

- **TCP Data Port**: データポート番号（デフォルト: 8888）
- **TCP Control Port**: 制御ポート番号（デフォルト: 8889）
- **TCP RX Buffer Size**: TCP受信バッファサイズ（デフォルト: 512バイト）

### WiFi 再試行回数の変更

`idf.py menuconfig` → `Component config` → `Example Configuration` → `Maximum WiFi connection retry`

デフォルト: 5回

### バッファサイズの変更

`main/main.c` の以下の定数を変更:
- `BUFFER_POOL_SIZE`: バッファプール内のバッファ数 (デフォルト: 16)
- `BUFFER_SIZE`: 各バッファのサイズ (デフォルト: 512バイト)

## トラブルシューティング

### WiFiに接続できない

1. プロビジョニング情報を消去して再設定:
   ```bash
   idf.py erase-flash
   idf.py flash monitor
   ```

2. WiFi設定を確認 (SSID、パスワード)

3. WiFi信号強度を確認

### USBデバイスが認識されない

1. USB接続を確認
2. USBデバイスが正常に動作していることをPCで確認
3. ESP32-S3のUSB OTGが有効になっていることを確認
4. デバッグログを有効化:
   ```bash
   idf.py menuconfig
   ```
   Component config → Log output → Default log verbosity → Debug

### TCPで接続できない

1. デバイスとクライアントが同じネットワーク上にあることを確認
2. ファイアウォール設定を確認
3. IPアドレスとポート番号を確認
4. `telnet <IP> 8888` で接続テスト

### mDNSでデバイスが見つからない

1. mDNSが有効なネットワークであることを確認
2. デバイスがWiFiに接続されていることを確認
3. mDNSクライアントツールが正しくインストールされていることを確認
4. シリアルモニタでmDNS初期化ログを確認

## コンポーネント

### FTDI SIO Host Driver
カスタムコンポーネント: `components/usb_host_ftdi_sio/`
- FTDI固有のUSB制御リクエスト
- デバイス管理とバルク転送
- FTDIチップタイプ検出

詳細は [components/usb_host_ftdi_sio/README.md](components/usb_host_ftdi_sio/README.md) を参照。

### ESP-IDF Managed Components
- `espressif/usb_host_cdc_acm`: CDC-ACMホストドライバ
- `espressif/network_provisioning`: WiFiプロビジョニング
- `espressif/mdns`: mDNSサービスディスカバリ

## 制限事項

- **シーケンシャル処理**: 一度に1つのUSBデバイスのみ処理
- **単一TCP接続**: 同時に1つのTCPクライアントのみサポート
- **マルチインターフェース非対応**: マルチポートFTDIデバイスは最初のインターフェースのみ使用
- **VID優先ルーティング**: VID `0x0403` のデバイスは常にFTDIドライバにルーティング

## ライセンス

このプロジェクトは Apache License 2.0 の下でライセンスされています。

個々のコンポーネントのライセンスについては、各コンポーネントのREADMEを参照してください。

## 参考資料

- [ESP-IDF USB Host ドキュメント](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_host.html)
- [USB CDC-ACM 仕様](https://www.usb.org/document-library/class-definitions-communication-devices-12)
- [FTDI チップ データシート](https://ftdichip.com/product-category/products/ic/)
- [ESP32-S3 USB OTG](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_host.html)
