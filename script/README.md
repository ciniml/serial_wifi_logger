# Scripts

このディレクトリには、ビルドとOTAアップデート用のスクリプトが含まれています。

## pack_firmware.py

単一のフラッシュイメージを生成するスクリプトです。リリースビルド時に自動的に実行されます。

**使用方法:**
```bash
idf.py build
python3 script/pack_firmware.py
```

**生成されるファイル:**
- `firmware.bin` - 単一フラッシュイメージ（0x0番地から書き込み可能）

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
