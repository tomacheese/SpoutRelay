# ビルド手順

## 前提条件

### コンパイラ

GCC 15.2.0 (MinGW-w64) を推奨します。[Scoop](https://scoop.sh/) でインストールできます。

```powershell
scoop install gcc
```

### ビルドツール

```powershell
scoop install cmake ninja
```

### 依存ライブラリの準備

`deps/` ディレクトリに以下が必要です。

#### FFmpeg 開発ライブラリ

[BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases) から `ffmpeg-master-latest-win64-lgpl-shared.zip` をダウンロードし、展開して `deps/ffmpeg/` に配置します。

```
deps/ffmpeg/
├── bin/          # avcodec-62.dll 等の DLL
├── include/      # libavcodec/ 等のヘッダー
└── lib/          # libavcodec.dll.a 等のインポートライブラリ
```

#### Spout2 SDK

[Spout2 GitHub](https://github.com/leadedge/Spout2) をクローンまたはダウンロードし `deps/Spout2/` に配置します。

```
deps/Spout2/
└── SPOUTSDK/
    ├── SpoutGL/          # SpoutCopy, SpoutDirectX, SpoutFrameCount 等
    └── SpoutDirectX/
        └── SpoutDX/      # SpoutDX.h / SpoutDX.cpp
```

#### nlohmann/json / spdlog（自動取得）

CMake の `FetchContent` により configure 時に自動でダウンロードされます。手動配置は不要です。

## ビルド手順

### 1. CMake 設定

```powershell
cd publisher
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake
```

`toolchain-mingw.cmake` は MSYS2/MinGW64 の標準インストールパスをデフォルトとしています。別の場所にインストールした場合は `MINGW_BIN` / `MINGW_ROOT` 環境変数で上書きできます。

### 2. ビルド（全ターゲット）

```powershell
cmake --build build
```

#### 個別ターゲット

```powershell
# メインアプリのみ
cmake --build build --target publisher-agent

# テストのみ
cmake --build build --target publisher_tests

# テスト用 Spout センダーのみ
cmake --build build --target spout_test_sender
```

### 3. FFmpeg DLL のコピー

ビルド完了後、`deps/ffmpeg/bin/` にある DLL を実行ファイルと同じ場所へコピーします。

```powershell
Copy-Item deps\ffmpeg\bin\*.dll publisher\build\
```

必要な DLL:

| DLL | 用途 |
|-----|------|
| `avcodec-62.dll` | H.264 エンコード |
| `avformat-62.dll` | RTSP マルチプレクサ |
| `avutil-60.dll` | FFmpeg ユーティリティ |
| `swscale-9.dll` | RGBA→YUV 変換 |
| `swresample-6.dll` | avformat の依存 |

## ビルド出力

```
publisher/build/
├── publisher-agent.exe       # メインアプリ
├── spout_test_sender.exe     # E2E テスト用 Spout センダー
└── tests/
    └── publisher_tests.exe   # ユニットテスト
```

## テスト実行

```powershell
.\publisher\build\tests\publisher_tests.exe
```

期待される出力:

```
All tests passed.
```

（31 tests: PASSED: 31, FAILED: 0）

## ツールチェーンのカスタマイズ

`toolchain-mingw.cmake` を編集してコンパイラパスを変更できます:

```cmake
set(CMAKE_C_COMPILER   "C:/path/to/gcc.exe")
set(CMAKE_CXX_COMPILER "C:/path/to/g++.exe")
```

## CMake ターゲット構成

| ターゲット | 種類 | 説明 |
|----------|------|------|
| `spout2_static` | 静的ライブラリ | Spout2 SDK（WIN32_LEAN_AND_MEAN なし） |
| `publisher_lib` | 静的ライブラリ | アプリロジック全体 |
| `publisher-agent` | 実行ファイル | メインアプリ |
| `publisher_tests` | 実行ファイル | カスタムテストランナーによるユニットテスト |
| `spout_test_sender` | 実行ファイル | E2E テスト用センダー |

### spout2_static の分離について

`SpoutFrameCount.cpp` はマルチメディアタイマー API（`TIMECAPS`、`timeBeginPeriod`）を使用しており、`WIN32_LEAN_AND_MEAN` が定義されていると `mmsystem.h` がインクルードされず、コンパイルエラーになります。このため Spout2 のソースは `WIN32_LEAN_AND_MEAN` を定義せずに別ターゲットとしてコンパイルしています。
