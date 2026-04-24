# Tailscale Client 技術ドキュメント

ESP32 向け Tailscale クライアント実装に必要な技術情報をまとめたドキュメント群。

## 目次

| ファイル | 内容 |
|---|---|
| [protocol_overview.md](protocol_overview.md) | プロトコル全体像・接続フロー |
| [ts2021_control.md](ts2021_control.md) | ts2021 コントロールプレーン (Noise IK + JSON API) |
| [derp_relay.md](derp_relay.md) | DERP リレープロトコル |
| [disco_protocol.md](disco_protocol.md) | DISCO 直接パス探索プロトコル |
| [key_management.md](key_management.md) | 鍵の種類・生成・NVS 管理 |
| [wireguard_integration.md](wireguard_integration.md) | WireGuard データプレーン統合 |
| [references.md](references.md) | 参考文献・リソース一覧 |

## 実装状況

| コンポーネント | ファイル | 状況 |
|---|---|---|
| 鍵管理 | `tailscale_keys.c` | 実装済み |
| Noise IK | `tailscale_noise.c` | 実装済み |
| コントロールプレーン | `tailscale_control.c` | 実装済み・動作確認中 |
| ネットワークマップ | `tailscale_netmap.c` | 実装済み |
| DERP リレー | `tailscale_derp.c` | 実装済み |
| DISCO | `tailscale_disco.c` | 実装済み |
