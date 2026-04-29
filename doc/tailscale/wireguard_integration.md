# WireGuard データプレーン統合

## 概要

Tailscale のデータプレーンは WireGuard。コントロールプレーン (ts2021) が
MapResponse からピア情報を取得し、WireGuard に動的に追加・削除する。

## 管理モード API

Tailscale 用に追加した動的ピア管理 API (`components/wireguard/include/wireguard_esp32.h`):

```c
// 管理モードで WireGuard インターフェースを起動 (ピア設定なし)
esp_err_t wireguard_esp32_start_managed(const char *local_ip,
                                         const char *local_netmask,
                                         const char *private_key_b64,
                                         uint16_t    listen_port);

// Tailscale IP アドレスを更新 (MapResponse 受信後)
esp_err_t wireguard_esp32_set_address(const char *ip_str,
                                       const char *netmask_str);

// ピアを動的追加
esp_err_t wireguard_esp32_add_peer(const char   *pub_key_b64,
                                    const ip_addr_t *allowed_ip,
                                    const ip_addr_t *allowed_mask,
                                    const ip_addr_t *endpoint_ip,
                                    uint16_t         endpoint_port,
                                    uint16_t         keepalive_sec,
                                    uint8_t         *peer_index_out);

// ピアのエンドポイント更新 (DISCO 直接パス確立時)
esp_err_t wireguard_esp32_update_endpoint(uint8_t   peer_index,
                                           const ip_addr_t *new_ip,
                                           uint16_t         new_port);

// ピアを削除
esp_err_t wireguard_esp32_remove_peer(uint8_t peer_index);

// 自分の WireGuard 公開鍵取得 (= Node Key)
esp_err_t wireguard_esp32_get_pubkey(uint8_t pubkey_out[32]);

// 自分の WireGuard 秘密鍵取得
esp_err_t wireguard_esp32_get_privkey(uint8_t privkey_out[32]);
```

## WireGuard ピア追加フロー

MapResponse の各 Peer エントリに対して:

```c
// 1. ピアの WireGuard 公開鍵 (raw → base64)
char pub_b64[64];
wireguard_base64_encode(peer_node_pub, 32, pub_b64, &b64_len);

// 2. Allowed IP = ピアの Tailscale IP /32
ip_addr_t allowed_ip, allowed_mask;
ip4addr_aton(ts_ip, &allowed4);
ip4addr_aton("255.255.255.255", &mask4);

// 3. Endpoint (あれば)
ip_addr_t ep_ip;
uint16_t ep_port;
parse_endpoint("1.2.3.4:41641", &ep_ip, &ep_port);

// 4. ピア追加
uint8_t wg_idx;
wireguard_esp32_add_peer(pub_b64, &allowed_ip, &allowed_mask,
                          &ep_ip, ep_port, 25, &wg_idx);
```

## ピア同期アルゴリズム

MapResponse が届くたびに全ピアリストを再同期する:

```
1. 受信したピアリストを走査:
   - 既存ピア (pubkey 一致): keep[] = true、エンドポイント変化あれば update
   - 新規ピア: add_peer()、keep[] = true
2. keep[] = false のピアを remove_peer() で削除
```

## WireGuard と Tailscale IP の関係

| 項目 | 値 |
|---|---|
| 自分の WireGuard IP | MapResponse の `Node.Addresses[0]` から取得 (例: `100.64.0.1`) |
| Netmask | `255.255.255.255` (/32) |
| Listen port | `CONFIG_TAILSCALE_LISTEN_PORT` (デフォルト: 41641) |
| ピアの Allowed IP | ピアの `Addresses[0]` /32 のみ |

## DERP 経由 WireGuard パケット

DERP で受信した WireGuard パケットは、WireGuard netif の `tcpip_input()` に
直接注入する (UDP ソケットを経由しない):

```c
// netif_list から "wg" netif を検索
struct netif *nif = netif_list;
while (nif && !(nif->name[0]=='w' && nif->name[1]=='g'))
    nif = nif->next;

// pbuf を確保して注入
struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
memcpy(p->payload, data, len);
tcpip_input(p, nif);  // WireGuard が復号・デカプセル化
```

## WIREGUARD_MAX_PEERS の設定

デフォルトは 1 (通常の WireGuard 用途)。Tailscale 使用時は 8 以上が必要。

`Kconfig` で `TAILSCALE_ENABLE=y` が `WIREGUARD_MAX_PEERS` を 8 に `select` する:

```kconfig
config TAILSCALE_ENABLE
    bool "Enable Tailscale"
    select WIREGUARD_ENABLE
    select WIREGUARD_MAX_PEERS_8  # 実装方針に依存
```

または `sdkconfig.defaults` で明示的に指定:
```
CONFIG_WIREGUARD_MAX_PEERS=8
```

## WireGuard netif の初期化タイミング

```
tailscale_esp32_start()
  └─ ts_keys_load()              ← NVS から鍵ロード
  └─ wireguard_esp32_start_managed()  ← WireGuard netif 起動 (IP は仮)
  └─ ts_disco_start()            ← DISCO UDP ソケット起動
  └─ ctrl_task (FreeRTOS task)
       └─ ts_ctrl_register()     ← RegisterResponse で Tailscale IP 取得
       └─ wireguard_esp32_set_address()  ← 正しい IP に更新
       └─ ts_ctrl_map_request()  ← ピア追加
       └─ ts_derp_set_home()     ← DERP 接続
       └─ ts_ctrl_poll_loop()    ← MapResponse ストリーム受信
```

## 実装上の注意

- WireGuard の `wireguardif_add_peer` は `struct wireguardif_peer` を初期化して渡す
- `wireguardif_peer.public_key` は base64 文字列 (raw ではない)
- ピア追加後に `wireguardif_connect(netif, peer_index)` を呼ぶ必要がある
- エンドポイントが `NULL` の場合 (DERP のみ)、`endpoint_ip = {0}` を渡す

## 参考文献

- [WireGuard 論文](https://www.wireguard.com/papers/wireguard.pdf) — データプレーン仕様
- [esp_wireguard](https://github.com/trombik/esp_wireguard) — 本実装のベース
- [lwIP netif API](https://www.nongnu.org/lwip/2_1_x/group__netif.html) — `tcpip_input()` など
- [tailcfg/tailcfg.go Peer 構造体](https://github.com/tailscale/tailscale/blob/main/tailcfg/tailcfg.go) — MapResponse のピアフィールド定義
