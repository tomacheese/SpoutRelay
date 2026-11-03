// Spout sender for the resolution-change test.
// Phase 1: send 640x360 for 15 seconds
// Phase 2: switch to 320x240 and send for 10 seconds
// Used to verify SpoutRelay's RECONFIGURING state transition.
//
// Usage: spout_res_changer.exe [sender_name]
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>
#include "SpoutDX.h"

static void fill_solid(std::vector<unsigned char>& buf,
                       unsigned int w, unsigned int h,
                       unsigned char r, unsigned char g, unsigned char b)
{
    buf.assign(static_cast<size_t>(w) * h * 4, 0);
    for (unsigned int i = 0; i < w * h; i++) {
        buf[i * 4 + 0] = b;   // BGRA order
        buf[i * 4 + 1] = g;
        buf[i * 4 + 2] = r;
        buf[i * 4 + 3] = 255;
    }
    // Draw an identification gradient bar at top-left
    for (unsigned int y = 0; y < 20 && y < h; y++)
        for (unsigned int x = 0; x < 100 && x < w; x++) {
            unsigned int idx = (y * w + x) * 4;
            unsigned char lum = static_cast<unsigned char>((x * 2) & 0xFF);
            buf[idx + 0] = buf[idx + 1] = buf[idx + 2] = lum;
            buf[idx + 3] = 255;
        }
}

int main(int argc, char* argv[]) {
    const char* name = (argc > 1) ? argv[1] : "TestSpoutSender";

    spoutDX sender;
    if (!sender.OpenDirectX11()) {
        fprintf(stderr, "[res_changer] Failed to open DirectX11\n");
        return 1;
    }

    // Clear orphaned entries of senders force-killed in a previous run.
    sender.sendernames.CleanSenders();
    sender.SetSenderName(name);

    // ---- Phase 1: 640x360 (blue) ----------------------------------------
    const unsigned int W1 = 640, H1 = 360;
    std::vector<unsigned char> buf1;
    fill_solid(buf1, W1, H1, 50, 80, 200);  // bluish

    printf("[res_changer] Phase 1: sending %ux%u (blue) for 15s...\n", W1, H1);
    fflush(stdout);

    for (int f = 0; f < 450; f++) {  // 30fps x 15s = 450 frames
        if (!sender.SendImage(buf1.data(), W1, H1))
            fprintf(stderr, "[res_changer] Phase1 SendImage failed at frame %d\n", f);
        if (f % 30 == 0)
            printf("[res_changer] phase1 frame %d / 240\r", f), fflush(stdout);
        Sleep(33);
    }

    // ---- Phase 2: 320x240 (green) - resolution change --------------------
    // SpoutDX updates the sender dimensions automatically when SendImage is
    // given a different width/height. The receiver (SpoutRelay) detects
    // IsUpdated()==true in ReceiveTexture()/ReceiveImage() and enters
    // RECONFIGURING via the freeze_meta.width comparison in encode_thread.
    const unsigned int W2 = 320, H2 = 240;
    std::vector<unsigned char> buf2;
    fill_solid(buf2, W2, H2, 50, 200, 80);  // greenish

    printf("\n[res_changer] Phase 2: sending %ux%u (green) for 10s...\n", W2, H2);
    fflush(stdout);

    for (int f = 0; f < 300; f++) {  // 30fps x 10s = 300 frames
        if (!sender.SendImage(buf2.data(), W2, H2))
            fprintf(stderr, "[res_changer] Phase2 SendImage failed at frame %d\n", f);
        if (f % 30 == 0)
            printf("[res_changer] phase2 frame %d / 300\r", f), fflush(stdout);
        Sleep(33);
    }

    printf("\n[res_changer] Done.\n");
    sender.ReleaseSender();
    return 0;
}
