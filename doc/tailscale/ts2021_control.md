# ts2021 コントロールプレーン

## 概要

ts2021 は Tailscale の制御チャネルプロトコル。`login.tailscale.com:443` に TLS 接続し、
Noise_IK_25519_ChaChaPoly_BLAKE2s でセッション鍵を確立した上で、JSON メッセージを
交換してデバイス登録・ネットワークマップ取得を行う。

## Step 1: サーバー公開鍵取得

```
GET /key?v=47 HTTP/1.1
Host: login.tailscale.com
Connection: keep-alive
```

### レスポンス (JSON)

```json
{
  "PublicKey": "nodekey:aabbccdd...",
  "LegacyPublicKey": "mkey:eeff0011..."
}
```

- `PublicKey` の値は `"nodekey:"` プレフィックス付き小文字 hex (64文字 = 32バイト)
- この公開鍵を Noise IK の responder static key として使用する
- TOFU (Trust On First Use) として NVS に保存し、2回目以降は照合する

## Step 2: Noise IK ハンドシェイク

### プロトコル仕様

```
プロトコル名:  Noise_IK_25519_ChaChaPoly_BLAKE2s
Prologue:      "ts2021" (6バイト)
identifier:    "Noise_IK_25519_ChaChaPoly_BLAKE2s\x00tailscale\x00ts2021\x00"
```

### 状態初期化

```
h  = BLAKE2s(identifier)
ck = BLAKE2s(construction_name)
MixHash(prologue)
MixHash(srv_pub)   ← サーバー静的公開鍵
```

### Initiator メッセージ (109 バイト)

```
[type=0x01 (1B)]
[eph_pub_i (32B)]          ← 毎回新規生成
[enc_static (48B)]         ← EncryptAndHash(s_pub_i)  after MixDH(eph_priv, srv_pub)
[enc_payload (28B)]        ← EncryptAndHash(version_info)  after MixDH(s_priv_i, srv_pub)
```

version_info (12バイト):
```c
{0x01,0x00,0x00,0x00,  // major = 1 (LE u32)
 0x00,0x00,0x00,0x00,  // minor = 0
 0x00,0x00,0x00,0x00}  // patch = 0
```

### HTTP POST でメッセージを送信

```
POST /ts2021 HTTP/1.1
Host: login.tailscale.com
Upgrade: tailscale-control-protocol
Connection: upgrade
Content-Length: 109

[109バイトの Noise init メッセージ]
```

サーバーは `101 Switching Protocols` で応答。

### Responder メッセージ (61 バイト、101直後に受信)

```
[type=0x02 (1B)]
[eph_pub_r (32B)]
[enc_payload (28B)]    ← EncryptAndHash(server_version_info 12B)
```

### セッション鍵導出

```
(send_key, recv_key) = HKDF_BLAKE2s(ck, "")
```

### HKDF_BLAKE2s (内部実装)

```
temp  = HMAC-BLAKE2s(ck, input_data)
out1  = HMAC-BLAKE2s(temp, 0x01)
out2  = HMAC-BLAKE2s(temp, out1 || 0x02)
```

## Step 3: RegisterRequest

ハンドシェイク後、Noise トランスポートフレームで JSON を送信。

### フレーム形式

```
[4-byte LE ciphertext_length][ChaChaPoly1305 ciphertext + 16B tag]
```

nonce は送受信それぞれ 0 から始まるカウンタ (uint64_t)。

### RegisterRequest (JSON)

```json
{
  "Version": 17,
  "NodeKey": "nodekey:hex...",
  "OldNodeKey": "nodekey:hex...",
  "Auth": {
    "AuthKey": "tskey-auth-xxx"
  },
  "Hostinfo": {
    "OS": "linux",
    "Hostname": "esp32-serial",
    "GoArch": "arm",
    "GoVersion": "go1.21"
  },
  "Endpoints": []
}
```

### RegisterResponse (JSON)

```json
{
  "User": {...},
  "Login": {...},
  "Node": {
    "ID": 12345,
    "Key": "nodekey:hex...",
    "Addresses": ["100.x.y.z/32"]
  },
  "AuthURL": ""   ← 空でなければ認証URLを開く必要あり
}
```

- `AuthURL` が非空の場合、デバイスはまだ承認されていない
- 承認後に再接続すると空になり、Tailscale IP が割り当てられる

## Step 4: MapRequest / MapResponse

### MapRequest

```json
{
  "Version": 17,
  "NodeKey": "nodekey:hex...",
  "Stream": true,
  "IncludeIPv6": false,
  "OmitPeers": false
}
```

`Stream: true` で long-poll ストリームになる。サーバーはネットワーク変化の度に
追加の MapResponse を送信し続ける。

### MapResponse (主要フィールド)

```json
{
  "Node": {
    "Addresses": ["100.x.y.z/32"],
    "Key": "nodekey:hex..."
  },
  "Peers": [
    {
      "Key": "nodekey:hex...",
      "Addresses": ["100.a.b.c/32"],
      "Endpoints": ["1.2.3.4:41641"],
      "DiscoKey": "discokey:hex...",
      "DERP": "127.3.3.40:1"
    }
  ],
  "DERPMap": {
    "Regions": {
      "1": {
        "RegionID": 1,
        "RegionCode": "tok",
        "Nodes": [
          {
            "Name": "tok1",
            "HostName": "derp1.tailscale.com",
            "DERPPort": 443,
            "STUNPort": 3478
          }
        ]
      }
    }
  }
}
```

## CapabilityVersion

`v=47` (最小サポートバージョン) を使用。
現在の Tailscale クライアントは `v=86` 程度だが、47 でも基本機能は動作する。

## 既知の問題・注意点

- `Connection: keep-alive` で GET /key と POST /ts2021 を同一 TLS セッションで行う
- サーバーはキープアライブを期待しているが、esp-tls は HTTP/1.1 keep-alive を
  自動的には管理しないため、GET 直後に POST を送れば問題ない
- RegisterResponse の `AuthURL` が非空の場合はシリアルログに URL を出力して
  ユーザーに認証を促す

## 参考文献

- [control/controlhttp/client.go](https://github.com/tailscale/tailscale/blob/main/control/controlhttp/client.go) — POST /ts2021、/key 取得フロー
- [control/controlclient/noise.go](https://github.com/tailscale/tailscale/blob/main/control/controlclient/noise.go) — Noise IK 実装
- [tailcfg/tailcfg.go](https://github.com/tailscale/tailscale/blob/main/tailcfg/tailcfg.go) — RegisterRequest/MapResponse 構造体定義
- [Noise Protocol Framework](https://noiseprotocol.org/noise.html) — IK パターン仕様
- [Noise Explorer IK](https://noiseexplorer.com/patterns/IK/) — ハンドシェイク状態遷移の可視化
