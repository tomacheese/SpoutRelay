# Spout RTSP Publisher

[Spout2](https://spout.zeal.co/) の GPU 共有テクスチャからフレームをキャプチャし、FFmpeg（NVENC → libx264 フォールバック）で H.264 エンコードして RTSP サーバー（[MediaMTX](https://github.com/bluenviron/mediamtx) 等）へ配信する Windows アプリケーションです。

## 特徴

- **ゼロコピー Spout キャプチャ** — SpoutDX 経由で DX11 共有テクスチャフレームを受信
- **ハードウェア H.264 エンコード** — NVIDIA NVENC 優先、ソフトウェア libx264 自動フォールバック
- **RTSP ANNOUNCE/RECORD** — TCP 経由で任意の RTSP サーバーへプッシュ配信
- **自動再接続** — エンコーダー・RTSP 障害時のエクスポネンシャルバックオフ
- **構造化 JSON ログ** — タイムスタンプ付き JSON Lines イベントログ
- **ランタイムメトリクス** — `health.json` / `metrics.json` を毎秒ディスクへ書き出し
- **ステートマシン駆動** — 明示的な状態遷移によるロバストな障害ハンドリング

## 動作要件

| コンポーネント | バージョン |
|--------------|----------|
| Windows | 10 / 11 (x64) |
| NVIDIA GPU（任意） | NVENC 対応カード |
| [FFmpeg 共有ライブラリ](https://github.com/BtbN/FFmpeg-Builds/releases) | win64-lgpl-shared, avcodec ≥ 61 |
| [Spout2 SDK](https://github.com/leadedge/Spout2) | `deps/` に同梱 |
| [MediaMTX](https://github.com/bluenviron/mediamtx) | v1.x（受信側） |
| GCC (MinGW-w64) | ≥ 13 推奨 |
| CMake | ≥ 3.20 |
| Ninja | 最新版 |

## クイックスタート

```bash
# 1. ディレクトリへ展開
cd publisher

# 2. MinGW ツールチェーンで CMake 設定
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake

# 3. ビルド
cmake --build build

# 4. FFmpeg DLL を実行ファイルと同じ場所へコピー
#    (avcodec-62.dll, avformat-62.dll, avutil-60.dll, swscale-9.dll, swresample-6.dll)

# 5. 設定ファイルを編集
cp config/config.example.json config/config.json
# spout.sender_name と rtsp.url を設定

# 6. MediaMTX（または任意の RTSP サーバー）を起動
deps/mediamtx/mediamtx.exe

# 7. 起動
build/publisher-agent.exe --config config/config.json
```

配信開始後、`rtsp://<サーバーIP>:8554/live` で VLC や FFplay から視聴できます。

## 使い方

```
publisher-agent.exe --config <config.json へのパス>
publisher-agent.exe --help
```

`Ctrl+C` または `CTRL_CLOSE_EVENT` でグレースフルシャットダウンします。

## 設定

完全なサンプル: [`config/config.example.json`](config/config.example.json)  
設定リファレンス: [`docs/configuration.md`](docs/configuration.md)

### 最小構成

```json
{
  "spout": { "sender_name": "YourSpoutSender" },
  "rtsp":  { "url": "rtsp://192.168.0.100:8554/live" }
}
```

## ディレクトリ構成

```
publisher/
├── src/
│   ├── app/          # Supervisor（オーケストレーター）+ StateMachine
│   ├── capture/      # FramePump – キャプチャスレッド＋有界キュー
│   ├── common/       # 型定義、エラーコード、時刻ユーティリティ
│   ├── config/       # JSON 設定ローダー
│   ├── encoder/      # FFmpeg H.264 エンコーダー（NVENC / libx264）
│   ├── logging/      # spdlog JSON Lines シンク
│   ├── metrics/      # MetricsStore → health.json / metrics.json
│   ├── rtsp/         # FFmpeg RTSP ANNOUNCE/RECORD クライアント
│   └── spout/        # SpoutDX レシーバー（SpoutMonitor）
├── tests/
│   ├── unit/         # カスタムテストランナーによるユニットテスト（22 テスト）
│   └── spout_test_sender/  # E2E テスト用カラーサイクル DX11 Spout センダー
├── config/
│   ├── config.example.json
│   └── config.json   # （.gitignore 対象、ローカル設定）
└── docs/             # カテゴリ別ドキュメント
```

## ドキュメント一覧

| ドキュメント | 内容 |
|-------------|------|
| [docs/architecture.md](docs/architecture.md) | システムアーキテクチャ、データフロー、スレッドモデル |
| [docs/build.md](docs/build.md) | 詳細ビルド手順・依存関係セットアップ |
| [docs/configuration.md](docs/configuration.md) | 設定項目の完全リファレンス |
| [docs/state-machine.md](docs/state-machine.md) | ステートマシン：状態・遷移・エラー処理 |
| [docs/metrics.md](docs/metrics.md) | メトリクス/ヘルス JSON フォーマット、イベントログ |
| [docs/troubleshooting.md](docs/troubleshooting.md) | よくあるエラーと解決策 |

## テストの実行

```bash
build/tests/publisher_tests.exe
# 期待結果: PASSED: 22, FAILED: 0
```

## ライセンス

本プロジェクトは以下を使用しています：
- [Spout2](https://github.com/leadedge/Spout2) — BSD-2-Clause
- [FFmpeg](https://ffmpeg.org/) — LGPL v2.1+（win64-lgpl-shared ビルド）
- [nlohmann/json](https://github.com/nlohmann/json) — MIT
- [spdlog](https://github.com/gabime/spdlog) — MIT
