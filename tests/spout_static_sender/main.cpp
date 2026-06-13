// 静的フリーズフレームテスト用 Spout 送信側
// 3 秒間だけフレームを送信し、その後は SendImage を止めて
// スリープし続ける（接続は維持したまま）。
// SpoutRelay がフリーズフレームを正しく送信し続けるか確認するのに使う。
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>
#include "SpoutDX.h"

int main(int argc, char* argv[]) {
    const char* name = (argc > 1) ? argv[1] : "TestSpoutSender";
    const unsigned int W = 640, H = 360;

    spoutDX sender;
    if (!sender.OpenDirectX11()) {
        fprintf(stderr, "[static_sender] Failed to open DirectX11\n");
        return 1;
    }
    // 前回実行で force-kill されたセンダーが Spout 名前リストに残っている場合、
    // SetSenderName() がインクリメント済み名前 (TestSpoutSender_1) を使うため
    // リレーが名前を見つけられなくなる。事前に孤立エントリを一掃しておく。
    sender.sendernames.CleanSenders();
    sender.SetSenderName(name);

    // 原色の赤（RGBA）で固定フレームを作る
    std::vector<unsigned char> buf(static_cast<size_t>(W) * H * 4);
    for (unsigned int i = 0; i < W * H; i++) {
        buf[i * 4 + 0] = 220;  // R
        buf[i * 4 + 1] = 50;   // G
        buf[i * 4 + 2] = 50;   // B
        buf[i * 4 + 3] = 255;  // A
    }
    // 左上にフレーム番号インジケーター（白のグラデーションバー）
    for (unsigned int y = 0; y < 20 && y < H; y++)
        for (unsigned int x = 0; x < 120 && x < W; x++) {
            unsigned int idx = (y * W + x) * 4;
            unsigned char lum = (unsigned char)((x * 2) & 0xFF);
            buf[idx + 0] = buf[idx + 1] = buf[idx + 2] = lum;
            buf[idx + 3] = 255;
        }

    // フェーズ 1: 10 秒間フレームを送信する（SpoutRelay 起動・接続の猶予を十分に確保）
    printf("[static_sender] Sending as '%s' (%ux%u) — 10 sec of frames...\n",
           name, W, H);
    for (int f = 0; f < 300; f++) {  // 30fps × 10s = 300 フレーム
        if (!sender.SendImage(buf.data(), W, H))
            fprintf(stderr, "[static_sender] SendImage failed at frame %d\n", f);
        if (f % 10 == 0)
            printf("[static_sender] sent frame %d\r", f), fflush(stdout);
        Sleep(33);
    }

    // フェーズ 2: SendImage を止めて接続だけ維持（IsConnected() = true のまま）
    printf("\n[static_sender] Stopped sending. Holding connection for 30 sec...\n");
    printf("[static_sender] SpoutRelay should keep streaming the last red frame.\n");
    fflush(stdout);
    Sleep(30000);  // 30 秒待機

    printf("[static_sender] Exiting.\n");
    sender.ReleaseSender();
    return 0;
}
