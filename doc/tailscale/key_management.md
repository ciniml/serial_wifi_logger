# 鍵管理

## 鍵の種類

| 名称 | 用途 | アルゴリズム | NVS キー |
|---|---|---|---|
| Machine Key | ts2021 Noise IK ハンドシェイク | curve25519 | `machine_priv` |
| Node Key | WireGuard 公開鍵・Tailscale ノード識別子 | curve25519 | `node_priv` |
| DISCO Key | DISCO パケット暗号化 | curve25519 | `disco_priv` |

すべて NVS namespace `"tailscale"` に 32-byte blob として保存。

## 鍵の生成

```c
// 1. 32バイトのランダム値を生成
wireguard_random_bytes(priv, 32);

// 2. RFC 7748 クランプ処理
priv[0]  &= 248;   // 最下位3ビットをクリア
priv[31] &= 127;   // 最上位ビットをクリア
priv[31] |= 64;    // 上位2ビット目をセット
```

## 公開鍵の導出

```c
static const uint8_t basepoint[32] = { 9 };  // curve25519 base point
wireguard_x25519(pub, priv, basepoint);
```

## NVS への保存・読み込み

```c
// 保存
nvs_handle_t h;
nvs_open("tailscale", NVS_READWRITE, &h);
nvs_set_blob(h, "machine_priv", priv, 32);
nvs_commit(h);
nvs_close(h);

// 読み込み
size_t len = 32;
nvs_get_blob(h, "machine_priv", priv, &len);
```

初回起動時はキーが存在しないため生成してから保存する。

## コントロールサーバー公開鍵 (TOFU)

| NVS キー | 型 | 内容 |
|---|---|---|
| `ctrl_srv_key` | blob(32) | login.tailscale.com の Noise 公開鍵 |

- 初回接続時に `/key?v=47` から取得して保存 (Trust On First Use)
- 2回目以降は NVS の値と照合し、不一致なら MITM として接続拒否

## キーの文字列表現

Tailscale は鍵をプレフィックス付き hex 文字列で表現する:

| 鍵の種類 | プレフィックス | 例 |
|---|---|---|
| Node Key (WireGuard) | `nodekey:` | `nodekey:aabb...ccdd` |
| DISCO Key | `discokey:` | `discokey:1122...3344` |
| Machine Key | `mkey:` | `mkey:5566...7788` |

### 変換関数

```c
// raw → "nodekey:hex..." 文字列
void ts_key_to_hex(const char *prefix,      // "nodekey:"
                   const uint8_t raw[32],
                   char *out,               // 出力バッファ (prefix + 64 + 1 以上)
                   size_t out_len);

// "nodekey:hex..." → raw 32バイト
bool ts_key_from_hex(const char *hex_str,   // プレフィックス付き文字列
                     uint8_t out[32]);       // 出力バッファ
```

## Auth Key (Pre-auth key)

Tailscale の pre-auth key は NVS または Kconfig から取得:

```
CONFIG_TAILSCALE_AUTH_KEY="tskey-auth-xxxxxxxxxxxxx"
```

RegisterRequest の `Auth.AuthKey` フィールドに設定することでデバイスが
自動的に承認される (手動承認不要)。

auth key を使わない場合は `AuthURL` が返されるのでブラウザで承認が必要。

## WireGuard 秘密鍵との関係

Node Key (秘密鍵) は WireGuard の private key としてもそのまま使用される。
WireGuard 管理モード (`wireguard_esp32_start_managed`) に base64 エンコードして渡す:

```c
// node_priv (raw 32B) → base64 文字列
size_t b64_len = 64;
wireguard_base64_encode(node_priv, 32, b64_str, &b64_len);

wireguard_esp32_start_managed(ts_ip, "255.255.255.255",
                               b64_str, CONFIG_TAILSCALE_LISTEN_PORT);
```

## 鍵ローテーション

現実装では鍵のローテーションは未実装。
NVS の blob を削除して再起動すれば新しいキーペアで再登録される。

## 参考文献

- [types/key/](https://github.com/tailscale/tailscale/tree/main/types/key) — Tailscale の鍵型定義 (nodekey/discokey/mkey プレフィックス)
- [RFC 7748 Section 5](https://www.rfc-editor.org/rfc/rfc7748#section-5) — curve25519 鍵クランプ処理の仕様
- [ESP-IDF NVS API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/storage/nvs_flash.html)
