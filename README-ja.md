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

### 実行時

| コンポーネント | 備考 |
|--------------|------|
| Windows 10 / 11 (x64) | 必須 |
| NVIDIA GPU | 任意 — NVENC ハードウェア H.264 が使用可能。なければ `h264_mf`（Windows Media Foundation）にフォールバック |
| FFmpeg 共有ライブラリ | リリース ZIP に同梱済み（`avcodec`、`avformat`、`avutil`、`swscale`、`swresample`） |
| RTSP サーバー | [MediaMTX](https://github.com/bluenviron/mediamtx) v1.x 等 — ストリームの受信側として別途起動が必要 |

### ビルド時

| コンポーネント | バージョン |
|--------------|----------|
| GCC (MinGW-w64) | ≥ 13 推奨 |
| CMake | ≥ 3.20 |
| Ninja | 最新版 |
| FFmpeg 開発ファイル | [win64-lgpl-shared](https://github.com/BtbN/FFmpeg-Builds/releases)、avcodec ≥ 62 |
| Spout2 SDK | git サブモジュールとして同梱（`deps/Spout2`） |

## クイックスタート

### 1. Releases からダウンロード

1. [最新リリース](https://github.com/tomacheese/SpoutRelay/releases/latest) を開く
2. `spout-relay-vX.Y.Z-win64.zip` をダウンロード
3. 任意のフォルダ（例: `C:\Tools\SpoutRelay\`）に展開

展開フォルダには以下が含まれます。

- `spout-relay.exe` — 本体実行ファイル
- `*.dll` — FFmpeg LGPL 共有ライブラリ（同じフォルダに置く必要があります）
- `config.example.json` — 設定テンプレート（コメント付き）
- `README.md`

### 2. 設定ファイルを作成

`config.example.json` を同じフォルダに `config.json` としてコピーし、最低限以下を設定します。

```json
{
  "spout": { "sender_name": "YourSpoutSender" },
  "rtsp":  { "url": "rtsp://<mediamtx-host>:8554/live" }
}
```

`YourSpoutSender` は Spout2 送信アプリ側で表示されている名前に、`<mediamtx-host>` は [MediaMTX](https://github.com/bluenviron/mediamtx) サーバーの IP アドレスまたはホスト名に置き換えてください。

### 3. 起動

```bat
spout-relay.exe --config config.json
```

配信が始まると `rtsp://<mediamtx-host>:8554/live` で視聴できます。  
VLC、FFplay などの RTSP 対応プレイヤーで確認できます。

```powershell
ffplay rtsp://<mediamtx-host>:8554/live
```

`Ctrl+C` でグレースフルシャットダウンします。

---

## ソースからビルド

```powershell
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
build/spout-relay.exe --config config/config.json
```

配信開始後、`rtsp://<サーバーIP>:8554/live` で VLC や FFplay から視聴できます。

## 使い方

```powershell
spout-relay.exe --config <config.json へのパス>
spout-relay.exe --help
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

```text
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
│   ├── unit/         # カスタムテストランナーによるユニットテスト（31 テスト）
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

```powershell
build/tests/publisher_tests.exe
# 期待結果: PASSED: 22, FAILED: 0
```

## ライセンス

このプロジェクトのライセンスは [MIT License](LICENSE) です。

本プロジェクトは以下を使用しています。

- [Spout2](https://github.com/leadedge/Spout2) — BSD-2-Clause
- [FFmpeg](https://ffmpeg.org/) — LGPL v2.1+（win64-lgpl-shared ビルド）
- [nlohmann/json](https://github.com/nlohmann/json) — MIT
- [spdlog](https://github.com/gabime/spdlog) — MIT
