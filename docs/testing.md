# テスト方法

SpoutRelay のテストは、以下の 4 つのカテゴリで構成されています。何らかの実装・改修を行った後は、
影響範囲に応じて該当するカテゴリのテストを **必ず網羅的に実施** してください。

| カテゴリ | 目的 | 実行環境 | 実行コマンド |
|---------|------|---------|-------------|
| [ユニットテスト](#ユニットテスト) | ロジック単体の正しさを検証 | CI / ローカル (GPU・Spout 不要) | `build/tests/publisher_tests.exe` |
| [E2E テスト](#e2e-テスト) | 実バイナリ・実 RTSP サーバーを用いた統合動作の検証 | ローカル (Windows + Spout/GPU 環境推奨) | `pwsh e2e-test/run-tests.ps1` |
| [モンキーテスト](#モンキーテスト) | 設計書を基にしたブラックボックスでの異常系・組み合わせ検証 | ローカル (Windows + Spout/GPU 環境推奨) | `pwsh e2e-test/monkey-tests.ps1` |
| [ユーザーテスト](#ユーザーテスト) | 実際の Spout ソース・実 RTSP サーバーでの最終確認 | 実機 (本番相当環境) | 手動 |

---

## ユニットテスト

### 概要

`tests/unit/` 以下にある各テストファイルは、CMake の `publisher_lib` をリンクした
`publisher_tests` 実行ファイルにまとめられ、GPU や Spout SDK の実体に依存しない
純粋なロジック・設定・状態遷移などを検証します。CI (GitHub Actions) でも実行されます。

独自の軽量テストフレームワーク (`tests/unit/test_utils.hpp`) を使用しており、
`VERIFY` / `VERIFY_MSG` マクロでアサーションを行います。失敗時は `std::exit(1)` で
即座に終了し、`[FAIL] ...` を標準エラー出力に表示します。

### テストファイル一覧

| ファイル | 内容 |
|---------|------|
| `tests/unit/test_config_loader.cpp` | 設定ファイル (`config.json`) のロード・バリデーションルール (エントリポイント `main()`) |
| `tests/unit/test_state_machine.cpp` | `StateMachine` の状態遷移ルール (有効/無効な遷移の組み合わせ) |
| `tests/unit/test_metrics_store.cpp` | `MetricsStore` のメトリクス集計・JSON 出力 |
| `tests/unit/test_backoff.cpp` | 再接続バックオフ計算ロジック |
| `tests/unit/test_placeholder_renderer.cpp` | NO SIGNAL プレースホルダ映像の生成ロジック |
| `tests/unit/test_log_sink.cpp` | JSON Lines ログ出力 (`LogSink`) のフォーマット・エスケープ・エラーハンドリング |
| `tests/unit/test_frame_pump.cpp` | `FramePump` のフレームキュー・キュー溢れ時の挙動・停止処理 |
| `tests/unit/test_supervisor_logic.cpp` | `Supervisor` から抽出した純粋ロジック (`supervisor_logic.hpp`) のシームレス切替判定・解像度変更検出 |

### 実行方法

```powershell
# ビルド (初回 or ソース変更後)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target publisher_tests -j 4

# 実行
.\build\tests\publisher_tests.exe
```

### 期待される出力

各テストブロックごとに `[PASS] ...` が出力され、最後に以下が表示されます。

```text
All tests passed.
```

PASS 件数はテストの追加に応じて変動します。1 件でも `[FAIL]` が出力された場合、
そのアサーションの内容と発生箇所 (ファイル名・行番号) が表示されて即座に終了します。

### 新規ロジック追加時の方針

- 新しい純粋関数 (副作用・スレッド・GPU/ファイル I/O に依存しないロジック) を追加する場合は、
  可能な限り `src/app/` 等の小さなヘッダ/関数として切り出し、対応するユニットテストを追加してください。
- `Supervisor` のように巨大なクラスに判定ロジックを直接書くと単体テストが困難になるため、
  `supervisor_logic.hpp` のように関数を抽出して `tests/unit/test_supervisor_logic.cpp` から検証する
  パターンを参考にしてください。
- 設定バリデーションを追加した場合は、`test_config_loader.cpp` に「不正な値を与えて
  `ConfigLoader::load()` が `false` を返すこと」を確認するテスト関数を追加してください。

---

## E2E テスト

### 概要

`e2e-test/` 以下の PowerShell 7+ スクリプトは、実際の `spout-relay.exe` バイナリ、
mediamtx (RTSP サーバー)、および Spout テスト送出元 (`spout_test_sender` 等) を起動し、
ステートマシンの遷移やストリーム配信の挙動を実際の RTSP ストリームを通じて検証します。

GPU エンコード (D3D11VA + NVENC) を利用する Spout/GPU 環境での実行を推奨しますが、
CPU フォールバック (libopenh264 / h264_mf) のテストも含まれます。

### 前提条件

- Windows + PowerShell 7+ (`pwsh`)
- `mediamtx`、`ffmpeg`/`ffprobe` が PATH 上にあること
- `spout-relay.exe`、`spout_test_sender.exe`、`spout_static_sender.exe`、
  `spout_res_changer.exe` がビルド済みであること (`build/` 以下)
- `e2e-test/config/` に各シナリオ用の設定ファイルが存在すること
  (`config.json`, `config_fatal.json`, `config_libopenh264.json`, `config_placeholder.json`)

### 実行方法

```powershell
cd e2e-test
pwsh ./run-tests.ps1       # Test 1-3 (RTSP 再接続・フリーズフレーム・長時間安定性)
pwsh ./monkey-tests.ps1    # Test 4-11 (異常系・組み合わせシナリオ一式)
pwsh ./run-new-tests.ps1   # Test 8-11 のみを個別実行 (monkey-tests.ps1 の一部を切り出したもの)
pwsh ./cli-tests.ps1       # CLI 引数・終了コード・Ctrl+C グレースフルシャットダウン
pwsh ./state-files-tests.ps1  # state/health.json・state/metrics.json の直接検証
pwsh ./multi-instance-tests.ps1  # 複数インスタンス同時起動時の競合確認
pwsh ./encoder-config-tests.ps1  # エンコーダー設定バリエーション (h264_mf 直接指定・極端なビットレート等)
```

各スクリプトは独立して `mediamtx` / `spout-relay` 等のプロセスを起動・停止するため、
実行前に既存プロセスが残っていないことを確認してください (スクリプト先頭の
`Stop-AllProcs` が自動的に後始末を行います)。

### テストシナリオ一覧

#### `run-tests.ps1` (Test 1-3)

| # | シナリオ | 検証内容 |
|---|---------|---------|
| 1 | RTSP 再接続直後の IDR 送出 (Bug #3) | mediamtx 再起動後、再接続時に IDR (I フレーム) が即座に送出されること |
| 2 | フリーズフレーム保持 (Bug #5') | 再接続後、直前のフレームを使ってストリームが即座に再開すること |
| 3 | 長時間配信の安定性 (Bug #7) | 15 秒間の連続配信でエンコードエラーが発生しないこと |

#### `monkey-tests.ps1` (Test 4-11)

| # | シナリオ | 検証内容 |
|---|---------|---------|
| 4 | Sender 切断 → STALLED → 復帰 | STREAMING 中に Spout ソースが消えた際 STALLED に遷移し、復帰後 STREAMING に戻ること |
| 5 | Placeholder モード | Sender 未接続時に NO SIGNAL 映像を配信し、Sender 接続後にシームレスにハンドオフすること |
| 6 | 連続 RTSP 再接続 (3 回) | mediamtx を 3 回連続で kill/再起動しても再接続できること |
| 7 | CPU コーデック (libopenh264) 直接使用 | GPU を使わない設定でも正常にエンコード・配信できること |
| 8 | STALLED → sender_disappeared → PROBING | プレースホルダ無効時、Sender 消失で PROBING に遷移し、復帰できること |
| 9 | GPU STREAMING → sender_disappeared → PLACEHOLDER シームレス | GPU モードでの Sender 消失時にシームレスに PLACEHOLDER へ切り替わること |
| 10 | 解像度変更 (640x360 → 320x240, RECONFIGURING) | `spout_res_changer` による解像度変更で RECONFIGURING を経て STREAMING に復帰すること |
| 11 | `max_reconnect_attempts` 超過 → FATAL | 再接続上限を超えた場合に FATAL 状態へ遷移すること |

#### `run-new-tests.ps1`

`monkey-tests.ps1` の Test 8-11 のみを単独実行するためのスクリプトです。
Test 8-11 のみを再検証したい場合に利用してください。

#### `cli-tests.ps1`

`main.cpp` の CLI 引数処理・終了コード・グレースフルシャットダウンを検証します。
mediamtx や Spout センダーは不要です。

| # | シナリオ | 検証内容 |
|---|---------|---------|
| 1 | `--help` | 使用方法を表示して終了コード 0 |
| 2 | `-h` | `--help` と同様 |
| 3 | 存在しない設定ファイル | 終了コード 1、`[ERROR]` を表示 |
| 4 | 不正な JSON の設定ファイル | 終了コード 1、`[ERROR]` を表示 |
| 5 | `-c` (短縮形) | `--config` と同様に動作 |
| 6 | 設定ファイル未指定 | デフォルトパス (`config/config.json`) が使われ、存在しなければ終了コード 1 |
| 7 | `Supervisor::init` 失敗 (`log_dir` がファイルに占有) | 終了コード 1、`[ERROR]` を表示 |
| 8 | `CTRL_BREAK_EVENT` 送信 | `console_ctrl_handler` がグレースフルシャットダウンし、終了コード 0 |

#### `state-files-tests.ps1`

`MetricsStore` が出力する `state/health.json` / `state/metrics.json` の
JSON 構造・値を実際のリレー実行を通じて直接検証します。

| # | シナリオ | 検証内容 |
|---|---------|---------|
| 1 | PLACEHOLDER 配信中 | `health.json` の `healthy`/`state`/`ts` フィールド、`state == "PLACEHOLDER"`、`healthy == true`、`ts` が有効な ISO 8601 であること。`metrics.json` の全フィールド存在、`state`/`rtsp_url`/`encoder_codec`/`uptime_ms`/`frames_encoded` の値が妥当であること |
| 2 | `max_reconnect_attempts` 超過 → FATAL | リレー終了後の `health.json` で `state == "FATAL"`、`healthy == false` であること |

#### `multi-instance-tests.ps1`

`instance_name` / `sender_name` / `rtsp.url` / `metrics_path` / `health_path` を
それぞれ変えた 2 つの spout-relay インスタンスを同一マシン上で同時起動し、
ログファイル・state ファイル・RTSP ストリームが競合・混在しないことを検証します。

| # | シナリオ | 検証内容 |
|---|---------|---------|
| 1 | 2 インスタンス同時起動 | それぞれ独立して `publish_started` (STREAMING) に到達すること |
| 2 | ログファイルの分離 | `{instance_name}.jsonl` がインスタンス毎に分離され、互いのイベントが混在しないこと |
| 3 | state ファイルの分離 | `metrics_*.json` / `health_*.json` がインスタンス毎に分離され、各自の `sender_name`/`rtsp_url` を反映すること |
| 4 | RTSP ストリームの独立性 | 両インスタンスの RTSP ストリームがそれぞれ個別に再生可能であること |

#### `encoder-config-tests.ps1`

`config.json` の `encoder`/`spout` セクションの値を変えた構成で、spout-relay が
正常に STREAMING に到達し RTSP ストリームを配信できることを検証します。

| # | シナリオ | 検証内容 |
|---|---------|---------|
| 1 | `codec: "h264_mf"` を直接指定 (フォールバックなし) | STREAMING に到達し配信できること |
| 2 | 極端に低いビットレート (`bitrate_kbps: 100`) | STREAMING に到達し配信できること |
| 3 | 極端に高いビットレート (`bitrate_kbps: 50000`) | STREAMING に到達し配信できること |
| 4 | `max_b_frames: 2` (h264_mf) | STREAMING に到達し配信できること |
| 5 | `prefer_dx11: false` | `prefer_dx11_ignored` 警告が出ても正常に配信できること |
| 6 | 低 FPS (`fps: 5`) | STREAMING に到達し配信できること |
| 7 | 高 FPS (`fps: 60`) | STREAMING に到達し配信できること |

### 期待される出力

各テストは `[PASS] ...` / `[FAIL] ...` を出力し、スクリプト終了時に
`Pass`/`Fail` カウントのサマリーが表示されます。`[FAIL]` が 1 件でもあれば
問題が修正されるまで対応してください。

---

## モンキーテスト

issue で定義されている「モンキーテスト」は、設計書 (`docs/` 以下のアーキテクチャ・
ステートマシン定義) を基に、ブラックボックスとして想定外の入力・タイミング・組み合わせを
試す探索的テストです。本リポジトリでは `e2e-test/monkey-tests.ps1` (Test 4-11) が
これに該当し、上記の E2E テストと同じ枠組み・実行コマンドで実施します。

新しい異常系シナリオ (例: 新しい状態遷移、新しい設定オプションの組み合わせ) を
追加実装した場合は、`monkey-tests.ps1` に対応するテストケースを追加してください。

### 手動で確認すべき追加シナリオ (自動化が困難なもの)

以下は実 RTSP サーバーの認証設定や、Spout 送出側で解像度を自由に変更できる
特殊なツールを要するため、現時点では自動 E2E では網羅できない項目です。
改修内容が以下に関係する場合は、手動でも動作確認してください。

- RTSP サーバー側で認証 (ユーザー名/パスワード) を要求する構成での接続・再接続
- RTSP サーバー側のタイムアウト設定変更時の挙動 (`connect_timeout_ms`/`send_timeout_ms` の境界値)
- RTSP サーバー側でのコーデック切替 (例: サーバー再起動で受理コーデックが変わる場合) 時の再接続
- 極端な解像度 (非常に小さい/大きい解像度、例: 64x64、3840x2160 超) の Spout ソース
- **GPU モード (`h264_nvenc` + `prefer_dx11: true`) での初回接続成功確認**:
  実 GPU 環境で `spout-relay.exe` を起動し、既に稼働中の Spout 送信元に接続した際に
  `logs/*.jsonl` に `SPOUT_RECEIVE_FAILED` が記録されず、直接 `CONNECTING_OUTPUT →
  STREAMING` へ遷移すること (`ReceiveTexture()` 無引数版の初回接続タイミングずれに
  起因する黒画面を修正した変更の確認)

これらは実 RTSP サーバーの設定変更や特殊な Spout 送出元が必要になるため、
対応する `e2e-test/config/` 設定ファイルを用意した上で `spout-relay.exe` を
手動起動し、`state/health.json` / `state/metrics.json` および `logs/*.jsonl`
を確認してください。

> **Note**: `h264_mf` 直接指定、極端なビットレート、`max_b_frames`、
> `prefer_dx11: false`、極端な FPS は `e2e-test/encoder-config-tests.ps1` で
> 自動検証されます。

- **`spout.stalled_recovery_max_attempts` ウォッチドッグの発動確認**:
  RTSP のみの再接続では回復しない STALLED ループを人工的に発生させることが難しいため、
  現時点では自動 E2E で網羅できていません。改修時は `stalled_recovery_max_attempts` を
  小さい値 (例: `2`) に設定した上で、意図的に `SpoutMonitor` 側の再接続を失敗させる
  デバッグビルド等で `stalled_recovery_forced` ログが記録され `PROBING` へ遷移することを
  手動で確認してください。

---

## 長時間耐久・ネットワーク異常試験 (手動手順)

以下は実行に長時間を要する、またはネットワーク環境の操作が必要なため、
CI / 通常の E2E テストには組み込まず、必要に応じて手動で実施する手順として
ここに記載します。エンコーダー・フレームパイプライン・RTSP 再接続ロジック・
メトリクス収集処理に大きな変更を加えた場合は、リリース前に実施してください。

### 長時間耐久試験 (メモリ/リソースリーク確認)

**目的**: 長時間 (数時間〜24 時間以上) 連続稼働させても、メモリ使用量や
ハンドル数が単調に増加し続けない (リークしない) こと、ログファイルの
ローテーションが正しく行われることを確認する。

**手順**:

1. `e2e-test/config/config.json` をベースに、ログローテーションが発生しやすいよう
   `app.log_dir` を指定した設定で `spout-relay.exe` を起動する
   (`LogSink` は 10MB/5 ファイルでローテーションするため、長時間稼働で
   実際にローテーションが発生することを確認できる)。
2. 実 Spout ソース (または `spout_test_sender.exe`) を接続し続け、
   RTSP クライアント (ffmpeg/VLC 等) で継続的に再生する。
3. 一定間隔 (例: 30 分おき) で以下を記録する:
   - `Get-Process spout-relay | Select WorkingSet, PrivateMemorySize, HandleCount`
   - `state/metrics.json` の `frames_received` / `frames_encoded` / `frames_dropped` /
     `rtsp_errors` / `reconnect_attempts` / `uptime_ms`
   - `logs/*.jsonl` のファイルサイズ・ローテーションされたファイル (`*.1`, `*.2`...) の有無
4. 数時間〜24 時間後、以下を確認する:
   - `WorkingSet` / `PrivateMemorySize` / `HandleCount` が単調増加し続けていないこと
     (多少の増加後、一定値に収束していること)
   - `frames_dropped` が継続的に増加し続けていないこと (フレームキューが
     正常に消費されていること)
   - ログファイルが `LogSink` の設定 (10MB/5 ファイル) どおりローテーションされ、
     ディスク使用量が無制限に増加していないこと
   - RTSP ストリームが途切れず再生できること

### ネットワーク異常 (パケットロス・帯域制限) 試験

**目的**: RTSP 送信経路でパケットロスや帯域制限が発生した場合に、
`RtspPublisherClient` の再接続ロジック (`reconnect_delay_ms` /
`reconnect_max_delay_ms` / `reconnect_backoff_multiplier` /
`max_reconnect_attempts`) が期待どおり動作し、ネットワークが復旧した際に
ストリームが正常に再開することを確認する。

**手順**:

1. Windows 上でパケットロス・帯域制限をシミュレートするツール
   (例: [clumsy](https://jagt.github.io/clumsy/)、`netsh interface tc` 相当の
   サードパーティツール、または WSL 上の `tc`/`netem`) を用意する。
2. `spout-relay.exe` と mediamtx を起動し、STREAMING (`publish_started`) に
   到達することを確認する。
3. RTSP サーバーへの通信経路に対して、以下のような条件を順に適用し、
   それぞれ `logs/*.jsonl` と `state/metrics.json` を観察する:
   - **軽度のパケットロス (5-10%)**: `rtsp_errors` が増加するが、ストリームが
     継続するか、短時間の再接続で復帰すること
   - **重度のパケットロス (30-50%)**: `state_changed` イベントで
     `STREAMING → RECONNECTING_OUTPUT` への遷移が記録され、
     `reconnect_attempts` が増加すること
   - **帯域制限 (設定ビットレートを下回る帯域)**: エンコーダーがエラーを
     出さずに動作を継続するか、`rtsp_errors`/再接続イベントが記録されること
   - **完全な遮断 → 復旧**: 通信を完全に遮断し、`max_reconnect_attempts` に
     達する前に復旧させた場合は `RECONNECTING_OUTPUT → STREAMING` に復帰すること。
     `max_reconnect_attempts` を超えるまで遮断を継続した場合は `FATAL` に
     遷移し、プロセスが終了すること (`e2e-test/monkey-tests.ps1` Test 11 と
     同様の確認)
4. 各条件適用後、ネットワーク条件を正常に戻し、ストリームが正常に
   再生できる状態に復帰することを確認する。

---

## ユーザーテスト

最終確認として、実際の Spout ソース (OBS, Resolume 等) と実 RTSP サーバーを用いて
動作確認を行います。

### 手順

1. 実機の Spout 送出元アプリケーションを起動し、SpoutRelay の `config.json` で
   対象の `sender_name` を指定します。
2. RTSP サーバー (mediamtx 等) を `rtsp://192.168.0.100:8554/spout-test` で
   配信可能な状態にしておきます。
3. `spout-relay.exe -c config.json` を起動します。
4. VLC 等の RTSP クライアントで `rtsp://192.168.0.100:8554/spout-test` に接続し、
   映像が正しく表示されることを確認します。
5. Spout 送出元を停止/再開し、NO SIGNAL プレースホルダ表示 (有効な場合) や
   再接続時の挙動を確認します。
6. `state/health.json` / `state/metrics.json` / `logs/*.jsonl` を確認し、
   期待される状態・メトリクスが記録されていることを確認します。

---

## 実装後のテストチェックリスト

何らかの実装・改修を行った場合は、以下のチェックリストに従って
影響範囲に応じたテストを実施してください。

- [ ] **ユニットテスト**: `build/tests/publisher_tests.exe` を実行し、`All tests passed.`
      が表示されること。ロジックを追加・変更した場合は対応するテストケースを追加していること。
- [ ] **E2E テスト**: ステートマシン・RTSP 配信・エンコーダーに関わる変更を行った場合、
      `pwsh e2e-test/run-tests.ps1` および `pwsh e2e-test/monkey-tests.ps1` を実行し、
      すべて `[PASS]` であること。
- [ ] **モンキーテスト**: 新しい異常系シナリオ・設定オプションを追加した場合、
      `monkey-tests.ps1` に対応するテストケースを追加し、実行結果が `[PASS]` であること。
- [ ] **ユーザーテスト**: 本番相当の挙動に影響する変更 (配信プロトコル、エンコード設定、
      Spout 接続処理など) を行った場合、実機の Spout ソース・RTSP サーバーで最終確認を行うこと。

すべての項目が完了し、問題が解消されていることを確認した上でコミット・PR 作成に進んでください。
