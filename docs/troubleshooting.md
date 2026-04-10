# トラブルシューティング

## 起動・設定エラー

### `[ERROR] Cannot open config file: config/config.json`

設定ファイルが見つかりません。`--config` オプションで絶対パスを指定してください。

```powershell
.\publisher-agent.exe --config C:\path\to\config\config.json
```

### 設定のバリデーションエラー（例: `spout.sender_name must not be empty`）

設定値が不正です。以下を確認してください:

- `spout.sender_name` が空でない
- `rtsp.url` が空でない
- `encoder.bitrate_kbps` > 0
- `encoder.fps` > 0

---

## Spout 関連

### `probe_no_sender` が続く / センダーが見つからない

**原因:** 指定した `sender_name` が Spout 共有メモリに登録されていない。

**確認方法:**

```powershell
$mmf = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting("SpoutSenderNames")
$accessor = $mmf.CreateViewAccessor()
$buf = New-Object byte[] (256 * 10)
$accessor.ReadArray(0, $buf, 0, $buf.Length)
for ($i = 0; $i -lt 10; $i++) {
    $name = [System.Text.Encoding]::ASCII.GetString($buf, $i*256, 256).TrimEnd([char]0)
    if ($name) { Write-Host "Sender: $name" }
}
```

取得したセンダー名を `config.json` の `spout.sender_name` に正確に設定してください。

### `SPOUT_RECEIVE_FAILED: Timeout waiting for first frame`（1回だけ発生）

**原因:** 初回接続時は `ReceiveImage()` が更新イベント（実データなし）を返すため、1 回リトライが発生します。これは正常動作です。2 回目の試行で通常は成功します。

### `SPOUT_RECEIVE_FAILED` が繰り返し発生する

**原因・対処:**

1. **GPU アダプター不一致** — Spout2 SDK の `SetAdapterAuto(true)` が有効になっているか確認（本プロジェクトでは自動設定済み）
2. **センダーがフレームを送出していない** — VRChat 等の送信側アプリが実際に動作しているか確認
3. **`frame_timeout_ms` が短すぎる** — GPU 負荷が高い場合は `300` → `500` 以上に増やす

---

## エンコーダー関連

### `ENCODER_INIT_FAILED`

**原因:** 設定したコーデック（`codec` と `fallback_codec` の両方）が使用できない。

**確認方法:**

```powershell
# NVENC が利用可能か確認
ffmpeg -encoders 2>$null | Select-String "nvenc"

# h264_mf（Windows Media Foundation）が利用可能か確認
ffmpeg -encoders 2>$null | Select-String "h264_mf"
```

NVENC が表示されない場合:
- NVIDIA GPU が搭載されていない、またはドライバーが古い
- FFmpeg が NVENC サポート付きでビルドされていない

h264_mf が表示されない場合:
- Windows Media Foundation が利用不可の環境（Server Core など）
- `codec` および `fallback_codec` の両方を使用環境に合わせて設定してください

### `avcodec_open2 failed: Option rc not found` (NVENC)

古い FFmpeg バージョンでの可能性があります。avcodec ≥ 62 の FFmpeg を使用してください。

### 色味がおかしい（赤と青が逆）

**原因:** SpoutDX の `ReceiveImage(bRGB=false)` は RGBA を出力しますが、エンコーダーが BGRA として処理している。

**修正:** `encoder_controller.cpp` の `sws_getContext` の入力フォーマットを `AV_PIX_FMT_BGRA` → `AV_PIX_FMT_RGBA` に変更し、再ビルドしてください（本プロジェクトでは修正済み）。

---

## RTSP 関連

### `RTSP_CONNECT_FAILED`

**確認事項:**

1. **MediaMTX が起動しているか**
   ```powershell
   Get-Process mediamtx -ErrorAction SilentlyContinue
   ```

2. **ネットワーク疎通確認**
   ```powershell
   Test-NetConnection -ComputerName 192.168.0.100 -Port 8554
   ```

3. **ファイアウォール** — MediaMTX が動作しているホストで TCP 8554 のインバウンドを許可しているか確認

4. **URL フォーマット** — `rtsp://` から始まり、ポート番号が正しいか確認

### `RTSP_SEND_FAILED` が頻発する

**原因・対処:**

- **ネットワーク帯域不足** — `encoder.bitrate_kbps` を下げる（例: 4000 → 2000）
- **送信タイムアウトが短い** — `rtsp.send_timeout_ms` を増やす（例: 5000 → 10000）
- **MediaMTX の設定** — `mediamtx.yml` の `writeQueueSize` を増やす

### 再接続が `max_reconnect_attempts` 回を超えて `FATAL` になる

`max_reconnect_attempts` の値を増やすか、MediaMTX を安定した環境で実行してください。

```json
"rtsp": {
  "max_reconnect_attempts": 10,
  "reconnect_max_delay_ms": 60000
}
```

---

## ビルド関連

### `SpoutFrameCount.cpp` のコンパイルエラー（`TIMECAPS` 未定義等）

`WIN32_LEAN_AND_MEAN` が定義された状態で Spout2 ソースをコンパイルすると `mmsystem.h` がインクルードされず、マルチメディアタイマー API が使えなくなります。

`CMakeLists.txt` で Spout2 ソースを `spout2_static` ターゲットとして `WIN32_LEAN_AND_MEAN` なしでコンパイルしていることを確認してください。

### `_mm_shuffle_epi8 was not declared in this scope`

`SpoutCopy.cpp` は SSSE3 命令を使用します。`CMakeLists.txt` に以下が設定されているか確認:

```cmake
set_source_files_properties(SpoutCopy.cpp PROPERTIES COMPILE_FLAGS "-mssse3 -msse4.1")
```

### リンクエラー: `GetFileVersionInfoA` が未解決

`version` ライブラリのリンクが必要です。`CMakeLists.txt` の `spout2_static` ターゲットに以下があることを確認:

```cmake
target_link_libraries(spout2_static PRIVATE version)
```

---

## E2E テスト手順

```powershell
# 1. MediaMTX 起動
Start-Process deps\mediamtx\mediamtx.exe

# 2. テスト用 Spout センダー起動（実際の Spout 送信側がない場合）
Start-Process publisher\build\spout_test_sender.exe

# 3. Publisher 起動
cd publisher\build
.\publisher-agent.exe --config ..\config\config.json

# 4. FFprobe で確認
ffprobe rtsp://192.168.0.100:8554/live

# 5. 5 秒間録画してフレーム数確認
ffmpeg -i rtsp://192.168.0.100:8554/live -t 5 -c copy test_out.mp4
ffprobe -v quiet -select_streams v:0 -show_entries stream=nb_read_frames -of csv test_out.mp4
```

正常動作: 5 秒で 140〜150 フレーム程度（30fps の場合）
