# アーキテクチャ

## システム概要

```text
┌─────────────────┐      DX11共有テクスチャ      ┌──────────────────────┐
│   Spout送信側    │ ─────────────────────────▶  │   SpoutMonitor       │
│ (VRChat等)      │      (GPU shared memory)     │   (SpoutDX)          │
└─────────────────┘                              └──────────┬───────────┘
                                                            │ RGBA フレーム
                                                            ▼
                                                 ┌──────────────────────┐
                                                 │   FramePump          │
                                                 │   (有界キュー 最大4)  │
                                                 └──────────┬───────────┘
                                                            │
                                                            ▼
                                                 ┌──────────────────────┐
                                                 │  EncoderController   │
                                                 │  NVENC → libx264     │
                                                 │  (FFmpeg + swscale)  │
                                                 └──────────┬───────────┘
                                                            │ H.264 NAL パケット
                                                            ▼
                                                 ┌──────────────────────┐
                                                 │  RtspPublisherClient │
                                                 │  ANNOUNCE / RECORD   │
                                                 │  (FFmpeg TCP)        │
                                                 └──────────┬───────────┘
                                                            │ RTP/TCP
                                                            ▼
                                                 ┌──────────────────────┐
                                                 │   MediaMTX (等)      │
                                                 │   RTSP サーバー      │
                                                 └──────────────────────┘
```

## コンポーネント説明

| コンポーネント | クラス | 役割 |
|-------------|--------|------|
| オーケストレーター | `Supervisor` | ステートマシン実行、全コンポーネントのライフサイクル管理 |
| ステートマシン | `StateMachine` | 状態遷移テーブルの強制、コールバック通知 |
| Spout 受信 | `SpoutMonitor` | SpoutDX による DX11 共有テクスチャ受信、RGBA ピクセル取得 |
| キャプチャキュー | `FramePump` | キャプチャスレッド実行、最大 4 フレームの有界キュー |
| エンコーダー | `EncoderController` | FFmpeg H.264 エンコード（NVENC/libx264）、swscale RGBA→YUV |
| RTSP クライアント | `RtspPublisherClient` | FFmpeg RTSP ANNOUNCE+SETUP+RECORD、RTP パケット送信 |
| メトリクス | `MetricsStore` | スレッドセーフなカウンター、health.json / metrics.json 出力 |
| ログ | `LogSink` | spdlog ベース JSON Lines ログ（ファイル＋コンソール） |
| 設定 | `ConfigLoader` | nlohmann/json による設定ロード・バリデーション |

## データフロー

```text
[SpoutDX GPU受信]
       │
       │  ReceiveImage() → RGBA バイト列
       ▼
[FrameBuffer (RGBA)]
       │
       │  push (非ブロッキング、満杯なら drop)
       ▼
[有界キュー (max=4)]
       │
       │  pop (エンコードスレッド)
       ▼
[sws_scale: RGBA → YUV420P/NV12]
       │
       │  avcodec_send_frame / avcodec_receive_packet
       ▼
[AVPacket (H.264 NAL)]
       │
       │  av_interleaved_write_frame
       ▼
[RTSP RTP/TCP → MediaMTX]
```

## スレッドモデル

```text
メインスレッド (Supervisor::run)
│
├── ステートマシンループ
│   └── 各状態ハンドラーが同期的にポーリング
│       (PROBING: 50ms, STREAMING: 33ms サイクル)
│
├── キャプチャスレッド (FramePump)
│   └── SpoutMonitor::receive_latest_frame() をポーリング
│       → FrameBuffer をキューへ push
│
└── メトリクスライタースレッド (Supervisor::metrics_writer_thread_func)
    └── 1秒ごとに health.json / metrics.json を書き出し
```

### スレッド間通信

| From | To | 手段 |
|------|----|------|
| キャプチャスレッド | ステートマシンループ | `std::queue` + `std::mutex` + `std::condition_variable` |
| ステートマシンループ | メトリクス | `std::atomic` カウンター + `std::mutex` 保護フィールド |
| `Supervisor::request_stop()` | 全スレッド | `std::atomic<bool> shutdown_requested_` |

## エンコーダーフォールバック

```text
avcodec_find_encoder("h264_nvenc")
      │
      ├── 成功 → avcodec_open2()
      │         │
      │         ├── 成功 → NVENC使用
      │         └── 失敗 → 次のコーデックへ
      │
      └── 失敗 → avcodec_find_encoder("libx264")
                        │
                        └── avcodec_open2() → libx264使用
```

コーデック固有オプション:

| コーデック | オプション |
|---------|---------|
| h264_nvenc | `rc=cbr`, `delay=0`, `preset=fast` |
| libx264 | `tune=zerolatency`, `zerolatency=1`, `preset=fast` |

## 依存ライブラリ

| ライブラリ | 用途 | 形式 |
|----------|------|------|
| Spout2 SDK | DX11 テクスチャ共有 | 静的リンク (`spout2_static`) |
| FFmpeg (avcodec/avformat/swscale) | エンコード・RTSP 配信 | 動的リンク (.dll) |
| nlohmann/json | 設定ファイルパース | ヘッダーオンリー |
| spdlog | 構造化ログ | ヘッダーオンリー |

## 既知の技術的制約

- **RTSP トランスポート**: 現状 TCP のみ（UDP 非対応）
- **ピクセルフォーマット**: SpoutDX からは RGBA で取得（`bRGB=false`）
- **Spout センダー名**: 設定ファイルに固定名を指定（自動検出なし）
- **初回フレーム取得**: 最初の `ReceiveImage()` は更新イベントとして扱われ実データなし（1 回リトライで解決）
