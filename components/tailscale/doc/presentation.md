---
marp: true
theme: default
size: 16:9
paginate: true
headingDivider: 2
header: ESP32 Tailscale Client
footer: (C) 2026 Kenta Ida
---

# ESP32にTailscaleクライアントを<br>自前実装した話

#### tailscaledが載らないなら、必要な所だけCで書く

<!--
_class: lead
_paginate: false
_header: ""
-->

<style>
img[alt~="center"] { display: block; margin: 0 auto; }
section { font-size: 26px; }
pre { font-size: 17px; }
</style>

<!--
3分LT。Serial WiFi Logger の続きとして、遠隔地からログを回収するために
ESP32単体でTailscaleに参加させた話をします。
-->

## 背景: IoT機器のログをリモートで回収したい

* 業務でESP32等のIoT機器を開発 → **動作中のログ収集**が課題
* LAN内は **Serial WiFi Logger** で解決済み
    * ターゲットのUSBシリアルを WiFi 経由で TCP に橋渡し

```
[ターゲット] --USB--> [Serial WiFi Logger] ~~WiFi~~> [PC]
   (ESP32等)            (ESP32-S3)                    nc / telnet
```

* **でも現場は遠隔地・NATの内側** → LANの外から安全に繋ぎたい
    * → VPNが欲しい

<!--
LAN内のログ収集はすでにできる。次の課題は、お客様先や遠隔地にある機器へ
NAT越しに、しかもセキュアに繋ぐこと。ここでVPNが要る。
-->

## なぜTailscale? / なぜ自前実装?

* **Tailscale**: WireGuardベースのVPN
    * NAT越え・デバイス認証・経路探索を「サクッと」やってくれる
* でも公式 `tailscaled` は Go製で数十MB → ESP32(RAM数百KB)には載らない

* そこで **必要最小限の機能だけ C で実装** (ESP-IDF 6.0)
    * トンネル本体は既存 `components/wireguard` を **managedモード**で再利用
    * Tailを「参加に必要な制御」だけに絞る

<!--
Tailscaleは便利だが本体はGo製で重くESP32に載らない。
WireGuardはすでに持っているので、足りない「コントロール部分」だけCで書いた。
-->

## 全体像: コントロール / データの分離

```
 ┌────────────┐  TLS   ┌─────────── tailscale component ───────────────┐
 │ Control     │◀──────▶│ control.c   keys.c (machine/node/disco鍵, NVS) │
 │ Server      │        │ (ts2021/H2)                                    │
 │ login.ts.com│        │      │ MapResponse(JSON)                       │
 └────────────┘        │  netmap.c ── ピア追加/削除/EP更新               │
 ┌────────────┐ relay  │      │                                         │
 │ DERP Server │◀──────▶│  derp.c (TLS中継)      disco.c (UDP経路探索)   │
 └────────────┘        │      └──────────┬──────────────┘               │
                       │  wireguard component (managed) = 暗号トンネル    │
                       └────────────────────────────────────────────────┘
```

* **コントロールプレーン**: IP・ピア一覧・DERP一覧(ネットマップ)を*配布*。鍵交換はしない
* **データプレーン**: WireGuardで暗号化通信。直接NGなら**DERP中継** → **DISCO**で直接へ昇格

<!--
Tailscaleはコントロールとデータが明確に分かれている。
コントロールは情報を配るだけ、実際の暗号通信はWireGuardが担う。
-->

## キモ① コントロール (ts2021)

```
 TLS (mbedTLS)
  └ HTTP/1.1 Upgrade  (POST /ts2021)
     └ Noise IK ハンドシェイク (Noise_IK_25519_ChaChaPoly_BLAKE2s)
        └ HTTP/2 (1フレーム = 1 Noiseレコード)
           └ JSON API: /machine/register, /machine/map (long-poll)
```

* この多段スタックを自前で実装(HTTP/2・HPACKも最小実装)
* **WireGuard連携のキモ**: `node鍵 = WireGuardの秘密鍵`
    * コントロールに登録する `NodeKey` と WGトンネルの公開鍵が一致
    * `/machine/map` の応答からピア表を WireGuard に同期

<!--
一番大変なのがここ。TLSの上にHTTP Upgrade、その上にNoise、さらにHTTP/2、
最後にJSON、と何層も重なっている。node鍵をWireGuardと共有するのが肝。
-->

## キモ② データ経路: DERP と DISCO

* **DERP**: 直接UDPが通らない時の中継 (常時HTTPS接続)
    * WireGuardで暗号化済 → **中継サーバは中身を復号できない**
* **DISCO**: 直接UDP経路を探索し、通れば**リレーから昇格** (低遅延・高帯域)

```
WG ──(127.3.3.40:region = DERP擬似EP)──▶ derp.c ──▶ DERP ──▶ 相手
DISCO Ping/Pong で直接経路が判明 ──▶ update_endpoint(実IP:port) ──▶ 直接UDP
```

* **WireGuardは経路を意識しない**: 擬似エンドポイント `127.3.3.0/24` で
  「直接UDPか中継か」を切り替えるだけ

<!--
最初はDERP中継で確実に繋ぎ、裏でDISCOが直接経路を探す。見つかれば
エンドポイントを実IPに差し替えるだけ。WG層は経路の違いを知らない。
-->

## 実装のポイントと現状

* **C 8ファイル**で構成 (control / netmap / noise / derp / disco / keys / jstream / esp32)
* 省メモリの工夫
    * cJSON不使用 → **SAX風ストリーミングJSONパーサ**でネットマップを解析
    * 暗号も自前: Noise IK・NaCl secretbox・XChaCha20-Poly1305
* **現状: 実機 (ESP32-S3) で動作確認済み**
    * ping / TCP・HTTP の疎通 OK、DERP中継 + DISCO直接経路

<!--
数十KBのJSONをツリー展開するとメモリが足りないので、SAX風で逐次処理。
実機ESP32-S3でping・TCP・HTTPまで通っている。
-->

## まとめ

* **ESP32単体で tailnet に参加**できる — 別途SBC(Raspberry Pi等)が不要
* 遠隔地のUSBシリアルログを **NAT越え・セキュア**に回収可能に
* tailscaled相当を「参加に必要な所だけ」Cで実装するのは現実的だった

* 実装解説: `components/tailscale/doc/architecture.md`
* https://github.com/ciniml/serial_wifi_logger

<!--
SBCを置かずESP32だけで遠隔地のログ回収ができるようになった。
詳しい実装はarchitecture.mdを参照。
-->
