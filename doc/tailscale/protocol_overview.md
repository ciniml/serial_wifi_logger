# Tailscale プロトコル全体像

## アーキテクチャ

```
┌─────────────────────────────────────────────────────────────────┐
│  ESP32 (Tailscale クライアント)                                   │
│                                                                   │
│  ┌──────────────┐   JSON over Noise IK   ┌─────────────────┐    │
│  │ コントロール  │ ◄────────────────────► │ login.tailscale │    │
│  │ プレーン     │      TLS/HTTPS          │  .com:443       │    │
│  │ (ts2021)    │                         └─────────────────┘    │
│  └──────┬───────┘                                                │
│         │ MapResponse (ピアリスト・DERPMap)                       │
│         ▼                                                         │
│  ┌──────────────┐   WireGuard UDP        ┌─────────────────┐    │
│  │ WireGuard    │ ◄────────────────────► │  DERP サーバー  │    │
│  │ データプレーン│   (直接 or DERP経由)   │  (中継)         │    │
│  └──────┬───────┘                        └─────────────────┘    │
│         │                                                         │
│         ▼                                                         │
│  ┌──────────────┐   UDP (DISCO)          ┌─────────────────┐    │
│  │ DISCO        │ ◄────────────────────► │  ピア (直接)    │    │
│  │ パス探索     │                         └─────────────────┘    │
│  └──────────────┘                                                │
└─────────────────────────────────────────────────────────────────┘
```

## 接続フロー

```
ESP32                          login.tailscale.com         DERP サーバー
  │                                   │                          │
  │──── TLS 接続 ──────────────────►  │                          │
  │──── GET /key?v=47 ──────────────► │                          │
  │◄─── {"PublicKey":"nodekey:..."} ── │                          │
  │                                   │                          │
  │  [Noise IK init msg を構築]        │                          │
  │                                   │                          │
  │──── POST /ts2021 ───────────────► │                          │
  │     Upgrade: tailscale-control-protocol                       │
  │     Content-Length: 109           │                          │
  │     [Noise IK 初期化メッセージ]    │                          │
  │◄─── 101 Switching Protocols ───── │                          │
  │◄─── [Noise IK レスポンス 61B] ─── │                          │
  │  [セッション鍵導出]                │                          │
  │                                   │                          │
  │──── RegisterRequest (暗号化) ───► │                          │
  │◄─── RegisterResponse (暗号化) ─── │  ← Tailscale IP 割り当て│
  │                                   │                          │
  │──── MapRequest (Stream=true) ───► │                          │
  │◄─── MapResponse ───────────────── │  ← ピアリスト・DERPMap  │
  │  [WireGuard ピア追加]              │                          │
  │  [DERP ホームサーバー決定]         │                          │
  │                                   │                          │
  │──── HTTPS GET /derp ────────────────────────────────────────►│
  │◄─── 101 Switching Protocols ────────────────────────────────-│
  │  [DERP 接続確立、WG パケット中継開始]                          │
  │                                   │                          │
  │──── DISCO Ping (UDP) ─────────────────────────────────────► peer
  │◄─── DISCO Pong (UDP) ───────────────────────────────────── peer
  │  [WireGuard エンドポイント更新 → 直接通信へ]                  │
```

## プロトコルスタック

| レイヤー | プロトコル | 実装ファイル |
|---|---|---|
| アプリ | RFC2217 / TCP | `main/` |
| VPN IP | Tailscale IP (100.x.y.z) | WireGuard netif |
| データプレーン | WireGuard | `wireguard_esp32.c` |
| NAT越え(中継) | DERP relay | `tailscale_derp.c` |
| NAT越え(直接) | DISCO + STUN | `tailscale_disco.c` |
| コントロール | ts2021 (Noise IK + JSON) | `tailscale_control.c` |
| 暗号 | Noise_IK_25519_ChaChaPoly_BLAKE2s | `tailscale_noise.c` |
| 鍵管理 | curve25519 keypairs / NVS | `tailscale_keys.c` |
| トランスポート | TLS (mbedTLS) / UDP | esp-tls / lwIP |

## 3種類の鍵

| 鍵の名前 | 用途 | 備考 |
|---|---|---|
| Machine Key | ts2021 Noise IK ハンドシェイク | コントロール接続の認証 |
| Node Key | WireGuard 公開鍵、Tailscale ノード識別 | MapResponse の `Key` フィールド |
| DISCO Key | DISCO パケットの暗号化 | UDP パス探索専用 |

すべて curve25519 (32-byte) キーペア。NVS に永続保存。

## 参考文献

- [Tailscale の仕組み (公式ブログ)](https://tailscale.com/blog/how-tailscale-works)
- [tailscale/tailscale (GitHub)](https://github.com/tailscale/tailscale)
- [Noise Protocol Framework](https://noiseprotocol.org/noise.html)
- → 詳細: [references.md](references.md)
