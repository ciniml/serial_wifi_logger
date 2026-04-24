# DISCO プロトコル

## 概要

DISCO (Direct IP/Subnet Connection) は Tailscale のピア間直接パス探索プロトコル。
UDP で DiscoPing/Pong を交換し、直接到達可能なエンドポイントを発見して
WireGuard のエンドポイントを DERP 経由から直接 UDP に切り替える。

## パケット形式

```
[6B magic][1B type][encrypted payload]
```

### マジックバイト

```
0x54 0x53 0xF0 0x9F 0x92 0xAC
= "TS💬" (UTF-8)
```

### パケットタイプ

| 値 | 名前 |
|---|---|
| `0x01` | DiscoPing |
| `0x02` | DiscoPong |
| `0x03` | CallMeMaybe |

## 暗号化

Disco パケットのペイロードは **XChaCha20-Poly1305 box** で暗号化される。

```
shared_secret = x25519(my_disco_priv, peer_disco_pub)
nonce = 24バイトランダム (DiscoPing ごとに新規生成)
box = XChaCha20-Poly1305(key=shared_secret, nonce=nonce, plaintext=payload)
```

実際のパケット:
```
[6B magic][1B type][24B nonce][sender_disco_pub 32B][ciphertext + 16B tag]
```

> **注意**: XChaCha20-Poly1305 は `xchacha20poly1305_encrypt/decrypt` 関数を使用。
> nonce は `const uint8_t *` 24バイト (WireGuard の ChaChaPoly nonce=uint64_t とは異なる)。

## DiscoPing フロー

```
ESP32 (initiator)                    Peer
  │                                    │
  │──── DiscoPing ──────────────────► │
  │     payload: {TxID[12B], MyAddr}  │
  │                                    │
  │◄─── DiscoPong ─────────────────── │
  │     payload: {TxID[12B], Src}     │
  │                                    │
  │  [WireGuard エンドポイント更新]     │
  │  DERP 経由 → 直接 UDP へ切替       │
```

### DiscoPing payload (12バイト以上)

```
[12B TxID]    ← ランダムな取引ID (照合用)
[可変 MyAddr] ← 送信者の見かけ上のアドレス (オプション)
```

### DiscoPong payload

```
[12B TxID]    ← 対応する Ping の TxID をそのまま返す
[可変 Src]    ← 送信者が見た Ping の送信元アドレス
```

## CallMeMaybe

DERP 経由でピアに UDP 接続を促すメッセージ。
ピアに「私のエンドポイントに Ping を送ってください」と伝える。

```json
{
  "MyNumber": ["1.2.3.4:41641", "[2001:db8::1]:41641"]
}
```

WireGuard パケットのペイロードとして DERP 経由で送られる場合もある。

## 実装上の注意

### ピア情報の取得元

DISCO の peer_disco_pub は MapResponse の `Peers[].DiscoKey` フィールドから取得:
```json
"DiscoKey": "discokey:aabbccdd..."
```
`"discokey:"` プレフィックスを除いた hex が 32バイトの公開鍵。

### エンドポイント更新

Pong 受信で直接パスが確立したら:
```c
wireguard_esp32_update_endpoint(peer_wg_index, &src_ip, src_port);
```

### Ping 送信先

MapResponse の `Peers[].Endpoints` に記載のエンドポイント:
```json
"Endpoints": ["1.2.3.4:41641", "192.168.1.100:41641"]
```

### ペンディングテーブル

Ping を送信してから Pong を受信するまでの間、TxID → peer の対応を保持するテーブルが必要。
現実装では最大 8 件。

### DERP 経由 DISCO

ピアへの UDP が通らない場合、DISCO パケットを WireGuard のペイロードとして
DERP 経由で送ることもできる (CallMeMaybe)。現実装では省略。

## 既知の制限

現実装では DISCO パケットの受信は UDP ソケット (port 41641) で待ち受けているが、
WireGuard が同じポートを使用している場合は競合するため、
WireGuard の listen port と DISCO の受信ソケットを正しく分離する必要がある。

実際の Tailscale クライアントでは、UDP パケットの先頭 6 バイトを見て
DISCO magic と WireGuard magic (4バイト: `0x01/0x02/0x03/0x04 + 3バイト`)
を区別している。WireGuard パケットの先頭は `0x01`〜`0x04` から始まるため
`0x54` から始まる DISCO マジックと衝突しない。

## 参考文献

- [disco/disco.go](https://github.com/tailscale/tailscale/blob/main/disco/disco.go) — マジックバイト・パケット型定義
- [magicsock/magicsock.go](https://github.com/tailscale/tailscale/blob/main/magicsock/magicsock.go) — DISCO 送受信・パス切替ロジック
- [XChaCha20-Poly1305 (draft-irtf-cfrg-xchacha)](https://datatracker.ietf.org/doc/html/draft-irtf-cfrg-xchacha) — 24バイト nonce の暗号仕様
