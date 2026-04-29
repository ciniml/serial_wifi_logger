# DERP リレープロトコル

## 概要

DERP (Detoured Encrypted Routing Protocol) は Tailscale の WireGuard パケット中継機構。
UDP が通らない NAT 環境でも通信できるよう、TLS 上の HTTP アップグレードで
バイナリフレームを中継する。

DERP サーバーのホスト名は MapResponse の `DERPMap` から取得する。

## 接続フロー

```
ESP32                           derp1.tailscale.com:443
  │                                       │
  │──── TLS 接続 ─────────────────────►  │
  │──── GET /derp HTTP/1.1 ────────────► │
  │     Host: derp1.tailscale.com         │
  │     Connection: upgrade               │
  │     Upgrade: DERP                     │
  │     Tailscale-Version: 1.56.0         │
  │◄─── 101 Switching Protocols ──────── │
  │                                       │
  │──── ClientInfo frame (type=0x02) ───► │  ← 自分の public key (32B) を送信
  │◄─── ServerInfo frame (type=0x03) ──── │
  │                                       │
  │  [接続確立]                            │
  │                                       │
  │──── SendPacket frame ───────────────► │  ← WG パケット送信
  │◄─── RecvPacket frame ──────────────── │  ← WG パケット受信
  │──── KeepAlive frame (25秒毎) ────────► │
```

## フレーム形式

```
[4-byte BE 合計長 (type + payload)][1-byte type][payload...]
```

合計長には type バイトを含む。payload の長さ = 合計長 - 1。

## フレームタイプ一覧

| 値 | 名前 | 方向 | payload |
|---|---|---|---|
| `0x01` | SendPacket | Client→Server | `[32B dst_pub_key][WG packet]` |
| `0x02` | ClientInfo | Client→Server | `[32B client_pub_key]` |
| `0x03` | ServerInfo | Server→Client | サーバー情報 (ドレインのみ) |
| `0x04` | RecvPacket | Server→Client | `[32B src_pub_key][WG packet]` |
| `0x06` | KeepAlive | 双方向 | なし (payload 0B) |
| `0x08` | PeerGone | Server→Client | `[32B departed_pub_key]` |

## WireGuard パケット注入

DERP で受信した WireGuard パケットは lwIP の `tcpip_input()` で WireGuard netif に
注入する。

```c
// WireGuard netif を netif_list から "wg" で検索
struct netif *nif = netif_list;
while (nif && !(nif->name[0] == 'w' && nif->name[1] == 'g'))
    nif = nif->next;

// pbuf を確保してパケットを注入
struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
memcpy(p->payload, data, len);
tcpip_input(p, nif);
```

## DERP サーバー選択

MapResponse の `DERPMap.Regions` から RegionID が最小のリージョンの最初のノードを
「ホームサーバー」として選択する。

```json
"DERPMap": {
  "Regions": {
    "1":  { "RegionCode": "tok", "Nodes": [...] },  ← RegionID=1 が最小
    "2":  { "RegionCode": "sin", "Nodes": [...] },
    "10": { "RegionCode": "nyc", "Nodes": [...] }
  }
}
```

実際の低遅延サーバー選択には STUN を使ったレイテンシ計測が必要だが、
現実装では最小 RegionID を使用している。

## 送信 API

```c
// DERP 経由で WireGuard パケットを送信
esp_err_t ts_derp_send(const uint8_t dst_pub[32],
                       const uint8_t *pkt, size_t pkt_len);
```

送信フレームのペイロード: `[32B dst_pub_key][WireGuard パケット]`

## KeepAlive

- 受信タスク内で 25 秒ごとにクライアントから KeepAlive を送信
- サーバーからの KeepAlive は受信しても何もしない（コネクション維持確認のみ）

## 再接続

- TLS セッションが切断された場合は `ts_derp_set_home()` を再呼び出しで再接続
- 同じホストに接続中であれば再接続しない

## スタック使用量

- 受信タスクスタック: 4096 bytes
- タスク優先度: 5
- 受信バッファ: 2084 bytes (最大 WG パケット + DERP ヘッダ)

## 参考文献

- [derp/derp.go](https://github.com/tailscale/tailscale/blob/main/derp/derp.go) — フレームタイプ定数・サーバー実装
- [derp/derphttp/derphttp_client.go](https://github.com/tailscale/tailscale/blob/main/derp/derphttp/derphttp_client.go) — HTTP upgrade・ClientInfo 送信フロー
- [Tailscale の仕組み (公式ブログ)](https://tailscale.com/blog/how-tailscale-works) — DERP の役割解説
