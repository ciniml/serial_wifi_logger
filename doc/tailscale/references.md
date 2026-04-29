# 参考文献・リソース

## Tailscale 公式

| リソース | URL | 備考 |
|---|---|---|
| Tailscale ソースコード (GitHub) | https://github.com/tailscale/tailscale | Go 実装の正典 |
| ts2021 プロトコル解説ブログ | https://tailscale.com/blog/how-tailscale-works | 全体アーキテクチャの概要 |
| DERP プロトコル解説 | https://tailscale.com/blog/how-tailscale-works#relaying | DERP の位置付け |
| Tailscale の Noise 実装 | https://github.com/tailscale/tailscale/blob/main/control/controlclient/noise.go | ts2021 Noise IK クライアント |
| DERP サーバー実装 | https://github.com/tailscale/tailscale/blob/main/derp/derp.go | フレーム形式の定義 |
| DERP クライアント実装 | https://github.com/tailscale/tailscale/blob/main/derp/derphttp/derphttp_client.go | HTTP upgrade フロー |
| DISCO 実装 | https://github.com/tailscale/tailscale/blob/main/disco/disco.go | パケット形式・暗号化 |
| tailcfg (MapRequest/Response 構造体) | https://github.com/tailscale/tailscale/blob/main/tailcfg/tailcfg.go | JSON フィールド定義 |
| controlhttp (HTTP upgrade) | https://github.com/tailscale/tailscale/blob/main/control/controlhttp/client.go | POST /ts2021 フロー |

## Noise プロトコル

| リソース | URL | 備考 |
|---|---|---|
| Noise Protocol Framework 仕様書 | https://noiseprotocol.org/noise.html | IK パターンの定義 |
| Noise Explorer (可視化) | https://noiseexplorer.com/patterns/IK/ | IK ハンドシェイクの状態遷移図 |

## 暗号プリミティブ

| リソース | URL | 備考 |
|---|---|---|
| RFC 7748 (curve25519/x448) | https://www.rfc-editor.org/rfc/rfc7748 | 鍵クランプ処理 (Section 5) |
| BLAKE2 仕様書 | https://www.blake2.net/blake2.pdf | BLAKE2s ハッシュ |
| ChaCha20-Poly1305 (RFC 8439) | https://www.rfc-editor.org/rfc/rfc8439 | AEAD 暗号 |
| XChaCha20-Poly1305 | https://datatracker.ietf.org/doc/html/draft-irtf-cfrg-xchacha | DISCO で使用、24B nonce |
| HKDF (RFC 5869) | https://www.rfc-editor.org/rfc/rfc5869 | Noise での利用は BLAKE2s ベース |

## WireGuard

| リソース | URL | 備考 |
|---|---|---|
| WireGuard 論文 | https://www.wireguard.com/papers/wireguard.pdf | データプレーン仕様の原典 |
| WireGuard プロトコル仕様 | https://www.wireguard.com/protocol/ | ハンドシェイク・トランスポート |
| esp_wireguard (本実装ベース) | https://github.com/trombik/esp_wireguard | ESP-IDF 向け WireGuard 実装 |

## ESP-IDF

| リソース | URL | 備考 |
|---|---|---|
| esp-tls API リファレンス | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/protocols/esp_tls.html | `esp_tls_conn_new_sync` など |
| mbedTLS 証明書バンドル | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/protocols/esp_crt_bundle.html | `esp_crt_bundle_attach` |
| NVS API | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/storage/nvs_flash.html | 鍵の永続化 |
| lwIP netif | https://www.nongnu.org/lwip/2_1_x/group__netif.html | WireGuard netif 操作 |

## 実装調査時のメモ

### `/key?v=47` レスポンス形式

Tailscale の `/key` エンドポイントは以下の JSON を返す:

```json
{
  "PublicKey": "nodekey:aabbcc...",
  "LegacyPublicKey": "mkey:eeff00..."
}
```

Go 側の構造体定義:
```go
// tailscale/control/controlhttp/server.go
type keyResponse struct {
    PublicKey    key.NodePublic
    LegacyPublicKey key.MachinePublic
}
```

`key.NodePublic` は JSON では `"nodekey:"` プレフィックス付き hex 文字列にシリアライズされる。

### Noise IK メッセージサイズの根拠

```
Initiator msg: 1 + 32 + (32+16) + (12+16) = 109 バイト
  type(1) + eph_pub(32) + enc_static(32+16) + enc_payload(12+16)

Responder msg: 1 + 32 + (12+16) = 61 バイト
  type(1) + eph_pub(32) + enc_payload(12+16)
```

payload は `major/minor/patch` バージョン各 4 バイト LE = 12 バイト。

Go 側の対応箇所: `tailscale/types/key/noise.go`, `tailscale/control/controlclient/noise.go`

### CapabilityVersion

`v=47` は 2021年頃の最小値。現在の Tailscale クライアントは `v=86` 以降を使用しているが、
基本的な Register/Map 機能は v=47 でも動作する。
高い capability version が必要な機能 (例: TS2024 暗号化) は使用しない。

### DERP フレーム長フィールドの解釈

公式実装 (`derp/derp.go`) より:
```go
// frameHeader は 5 バイト: [4B BE フレーム本体長][1B フレームタイプ]
// フレーム本体長 = frameType(1B) + payload のバイト数
// つまり payload 長 = フレーム本体長 - 1
```

受信時: `payload_len = (hdr[0]<<24|hdr[1]<<16|hdr[2]<<8|hdr[3]) - 1`
