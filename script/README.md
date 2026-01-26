# Scripts

このディレクトリには、ファームウェアのビルド、フラッシュ、OTAアップデート用のスクリプトが含まれています。

## pack_firmware.py

単一のフラッシュイメージを生成するスクリプトです。リリースビルド時に自動的に実行されます。

**使用方法:**
```bash
idf.py build
python3 script/pack_firmware.py
```

**生成されるファイル:**
- `firmware.bin` - 単一フラッシュイメージ（0x0番地から書き込み可能）

## flash_firmware.sh

GitHubリリースから自動的にファームウェアをダウンロードして、ESP32-S3にフラッシュするスクリプトです。初回セットアップ時に使用します。

### 必要な依存関係

```bash
# Ubuntu/Debian
sudo apt-get install curl jq unzip python3-pip
pip3 install esptool

# macOS
brew install curl jq unzip
pip3 install esptool
```

### 使用方法

**基本的な使い方:**
```bash
# ヘルプを表示
./script/flash_firmware.sh --help

# 最新バージョンをフラッシュ（ポート自動検出）
./script/flash_firmware.sh

# 特定バージョンをフラッシュ
./script/flash_firmware.sh v1.0.0

# ポートを指定
./script/flash_firmware.sh v1.0.0 /dev/ttyUSB0

# ボーレートも指定
./script/flash_firmware.sh latest /dev/ttyUSB0 921600
```

**具体例:**
```bash
# Linux - 最新バージョン、自動検出
./script/flash_firmware.sh

# Linux - 特定バージョン、ポート指定
./script/flash_firmware.sh v0.2.0 /dev/ttyUSB0

# macOS
./script/flash_firmware.sh v0.2.0 /dev/cu.usbserial-0001

# Windows (Git Bash)
./script/flash_firmware.sh v0.2.0 COM3
```

**実行例:**
```bash
$ ./script/flash_firmware.sh v0.2.0 /dev/ttyUSB0
[INFO] Flash Firmware Tool v1.0.0
[INFO] Repository: ciniml/serial_wifi_logger
[INFO] Firmware version: v0.2.0
[INFO] Target chip: esp32s3

[INFO] Using esptool: esptool.py
[INFO] Fetching release information for tag: v0.2.0
[INFO] Found release: v0.2.0 (v0.2.0)
[INFO] Downloading firmware from: https://github.com/.../firmware-v0.2.0.zip
######################################################################## 100.0%
[SUCCESS] Downloaded firmware ZIP: 512K
[INFO] Extracting firmware...
[SUCCESS] Extracted firmware: 1.0M
[INFO] Verifying ESP32-S3 connection...
[SUCCESS] ESP32-S3 detected
[INFO] Flashing firmware to ESP32-S3...
[INFO] Port: /dev/ttyUSB0
[INFO] Baud rate: 460800
[INFO] Firmware: /tmp/flash_firmware_12345/firmware-v0.2.0.bin (1.0M)

[WARNING] Make sure ESP32-S3 is in download mode (press and hold BOOT button, then press RESET)
[WARNING] Press Enter to continue or Ctrl+C to cancel...

[INFO] Starting flash process...

esptool.py v4.7.0
Serial port /dev/ttyUSB0
Connecting....
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded Flash 8MB (XMC)
Crystal is 40MHz
MAC: 34:85:18:aa:bb:cc
Uploading stub...
Running stub...
Stub running...
Configuring flash size...
Flash will be erased from 0x00000000 to 0x001effff...
Compressed 2031616 bytes to 1234567...
Wrote 2031616 bytes (1234567 compressed) at 0x00000000 in 30.5 seconds
Verifying...
Hash of data verified.

Leaving...
Hard resetting via RTS pin...

[SUCCESS] Flash completed successfully!
[INFO] You can now reset the ESP32-S3 (press RESET button)
[SUCCESS] Firmware flash completed successfully!
[INFO] Next steps:
[INFO]   1. Press RESET button on ESP32-S3
[INFO]   2. Connect to WiFi provisioning AP (serial-XXXXXX)
[INFO]   3. Configure WiFi credentials
[INFO]   4. Access Web UI at http://serial-XXXXXX.local/
```

### 環境変数

**GITHUB_TOKEN**

プライベートリポジトリの場合、GitHubパーソナルアクセストークンを設定してください。

```bash
export GITHUB_TOKEN="ghp_xxxxxxxxxxxxxxxxxxxx"
./script/flash_firmware.sh
```

**ESPTOOL**

カスタムesptoolパスを指定できます。

```bash
export ESPTOOL="/path/to/custom/esptool.py"
./script/flash_firmware.sh
```

### トラブルシューティング

#### esptool.pyが見つからない

```
[ERROR] esptool.py not found
[INFO] Install with: pip install esptool
```

**対処法:**
```bash
# Python 3がインストールされていることを確認
python3 --version

# esptoolをインストール
pip3 install esptool

# または
python3 -m pip install esptool
```

#### ESP32-S3に接続できない

```
[ERROR] Failed to communicate with ESP32-S3
```

**対処法:**
1. USB接続を確認
2. ドライバがインストールされているか確認（Windows）
3. ダウンロードモードに入る:
   - BOOTボタンを押し続ける
   - RESETボタンを押して離す
   - BOOTボタンを離す

#### ポートが自動検出されない

```
[ERROR] No serial ports detected
```

**対処法:**
```bash
# Linuxでポートを確認
ls -la /dev/ttyUSB* /dev/ttyACM*

# macOSでポートを確認
ls -la /dev/cu.usbserial* /dev/cu.SLAB*

# Windowsでポートを確認（デバイスマネージャー）
# COMポート番号を確認

# 手動でポートを指定
./script/flash_firmware.sh v0.2.0 /dev/ttyUSB0
```

#### 書き込み速度が遅い

デフォルトのボーレート（460800）で問題がある場合、速度を変更できます:

```bash
# より高速（安定性は低下する可能性）
./script/flash_firmware.sh v0.2.0 /dev/ttyUSB0 921600

# より安定（速度は低下）
./script/flash_firmware.sh v0.2.0 /dev/ttyUSB0 115200
```

### スクリプトの機能

1. **依存関係チェック**: 必要なツール（curl, jq, unzip, esptool.py）が利用可能か確認
2. **リリース情報取得**: GitHub APIから指定バージョン（またはlatest）のリリース情報を取得
3. **ファームウェアダウンロード**: リリースからZIPファイルをダウンロード
4. **ファームウェア抽出**: ZIPから単一フラッシュイメージ（`firmware-vX.X.X.bin`）を抽出
5. **ポート検出**: シリアルポートを自動検出（または手動指定）
6. **チップ検証**: ESP32-S3との通信を確認
7. **フラッシュ**: esptool.pyを使用してフラッシュ
8. **クリーンアップ**: 一時ファイルを削除

## ota_update.sh

GitHubリリースから自動的にファームウェアをダウンロードして、デバイスにOTAアップデートを実行するスクリプトです。

### 必要な依存関係

```bash
# Ubuntu/Debian
sudo apt-get install curl jq unzip

# macOS
brew install curl jq unzip
```

### 使用方法

**基本的な使い方:**
```bash
# ヘルプを表示
./script/ota_update.sh --help

# 最新バージョンにアップデート
./script/ota_update.sh <device-hostname-or-ip>

# 特定バージョンにアップデート
./script/ota_update.sh <device-hostname-or-ip> <version-tag>

# デバッグモードで実行
DEBUG=1 ./script/ota_update.sh <device-hostname-or-ip> <version-tag>
```

**具体例:**
```bash
# mDNSホスト名を使用（推奨）
./script/ota_update.sh serial-A02048.local

# 最新バージョンを明示的に指定
./script/ota_update.sh serial-A02048.local latest

# 特定バージョンを指定
./script/ota_update.sh serial-A02048.local v1.0.0

# IPアドレスを使用
./script/ota_update.sh 192.168.1.100

# IPアドレスと特定バージョンを指定
./script/ota_update.sh 192.168.1.100 v1.0.0
```

### 実行例

```bash
$ ./script/ota_update.sh serial-A02048.local
[INFO] OTA Firmware Update Tool
[INFO] Target device: serial-A02048.local
[INFO] Version: latest

[INFO] Fetching device information from http://serial-A02048.local/api/info

════════════════════════════════════════
  Device Information
════════════════════════════════════════
  Address:    serial-A02048.local
  Version:    0.1.0 g5f9ddec DEV
  Partition:  ota_0
  Uptime:     3600s
════════════════════════════════════════

Continue with OTA update? (y/N): y
[INFO] Fetching latest release information from GitHub...
[INFO] Found release: Serial WiFi Logger v1.0.0 (v1.0.0)
[INFO] Downloading firmware from: https://github.com/ciniml/serial_wifi_logger/releases/download/v1.0.0/firmware-v1.0.0.zip
######################################################################## 100.0%
[SUCCESS] Downloaded firmware ZIP: 512K
[INFO] Extracting firmware...
[SUCCESS] Extracted firmware: 931K
[INFO] Uploading firmware to serial-A02048.local...
[INFO] This may take 30-60 seconds depending on WiFi speed
######################################################################## 100.0%

[SUCCESS] Firmware uploaded successfully!
[INFO] Device will reboot in 3 seconds...
[INFO] Waiting for device to restart...
[INFO] Verifying firmware update...
[INFO] Waiting for device to fully boot (15 seconds)...

════════════════════════════════════════
  Updated Device Information
════════════════════════════════════════
  Version:    1.0.0 g5f9ddec RELEASE
  Partition:  ota_1
════════════════════════════════════════

[SUCCESS] Device is running with new firmware!
[SUCCESS] OTA update completed successfully!
[INFO] Web UI: http://serial-A02048.local/
```

### 環境変数

**GITHUB_TOKEN**

プライベートリポジトリの場合、GitHubパーソナルアクセストークンを設定してください。

```bash
export GITHUB_TOKEN="ghp_xxxxxxxxxxxxxxxxxxxx"
./script/ota_update.sh serial-A02048.local
```

### デバッグモード

問題が発生した場合、デバッグモードで詳細な情報を確認できます。

```bash
DEBUG=1 ./script/ota_update.sh serial-A02048.local v0.2.0
```

デバッグモードでは以下の情報が表示されます:
- 実行される全てのコマンド
- API呼び出しの詳細
- 変数の値
- エラーの発生場所

### トラブルシューティング

#### スクリプトのバージョン確認

スクリプトのバージョンとリポジトリ情報を確認:
```bash
./script/ota_update.sh 192.168.1.100
# 出力の最初の行でバージョンを確認
# [INFO] OTA Firmware Update Tool v1.0.2
# [INFO] Repository: ciniml/serial_wifi_logger
```

#### デバイスに接続できない

```
[WARNING] Failed to connect to device at serial-A02048.local
```

**対処法:**
1. デバイスが同じネットワークに接続されているか確認
2. mDNSが利用可能か確認（`ping serial-A02048.local`）
3. mDNSが使えない場合はIPアドレスを直接指定

#### GitHubリリースが見つからない

```
[ERROR] GitHub API error: Not Found
[ERROR] Release tag 'v1.0.0' not found
```

**対処法:**
1. 指定したバージョンタグが存在するか確認
2. リポジトリのReleasesページを確認
3. `latest`を指定して最新版を取得

#### ファームウェアのアップロードに失敗

```
[ERROR] Upload failed!
[ERROR] HTTP Status: 400
```

**対処法:**
1. デバイスのストレージに十分な空き容量があるか確認
2. WiFi接続が安定しているか確認
3. デバイスのログを確認（`idf.py monitor`）
4. デバイスを再起動して再試行

#### 依存関係がインストールされていない

```
[ERROR] Missing required dependencies: jq unzip
```

**対処法:**
```bash
# Ubuntu/Debian
sudo apt-get install curl jq unzip

# macOS
brew install curl jq unzip
```

### セキュリティに関する注意

- スクリプトは一時ディレクトリ（`/tmp/ota_update_$$`）を使用します
- ダウンロードしたファイルは実行後に自動的に削除されます
- HTTPS経由でGitHubからファームウェアをダウンロードします
- デバイスへのアップロードはHTTP経由です（ローカルネットワーク内）

本番環境でセキュアな通信が必要な場合は、HTTPS対応を検討してください。

### スクリプトの動作

1. **依存関係チェック**: 必要なコマンド（curl, jq, unzip）が利用可能か確認
2. **デバイス情報取得**: アップデート前のバージョンとパーティション情報を表示
3. **リリース情報取得**: GitHub APIから指定バージョン（またはlatest）のリリース情報を取得
4. **ファームウェアダウンロード**: リリースからZIPファイルをダウンロード
5. **ファームウェア抽出**: ZIPから`serial_wifi_logger.bin`を抽出
6. **アップロード**: HTTPポスト経由でデバイスにファームウェアをアップロード
7. **検証**: デバイスの再起動後、新しいバージョンで起動したか確認
8. **クリーンアップ**: 一時ファイルを削除
