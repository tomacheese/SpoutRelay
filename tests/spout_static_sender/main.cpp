// Spout sender for the static freeze-frame test.
// Sends frames for the first 10 seconds, then stops calling SendImage
// and just sleeps (the connection stays up).
// Used to check that SpoutRelay keeps sending the freeze frame correctly.
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
    // If a sender force-killed in a previous run is still in the Spout name list,
    // SetSenderName() picks the incremented name (TestSpoutSender_1) and
    // the relay can no longer find the sender. Clear orphaned entries first.
    sender.sendernames.CleanSenders();
    sender.SetSenderName(name);

    // Build a fixed frame in primary red (RGBA)
    std::vector<unsigned char> buf(static_cast<size_t>(W) * H * 4);
    for (unsigned int i = 0; i < W * H; i++) {
        buf[i * 4 + 0] = 220;  // R
        buf[i * 4 + 1] = 50;   // G
        buf[i * 4 + 2] = 50;   // B
        buf[i * 4 + 3] = 255;  // A
    }
    // Frame-number indicator at top-left (white gradient bar)
    for (unsigned int y = 0; y < 20 && y < H; y++)
        for (unsigned int x = 0; x < 120 && x < W; x++) {
            unsigned int idx = (y * W + x) * 4;
            unsigned char lum = (unsigned char)((x * 2) & 0xFF);
            buf[idx + 0] = buf[idx + 1] = buf[idx + 2] = lum;
            buf[idx + 3] = 255;
        }

    // Phase 1: send frames for 10 seconds (ample time for SpoutRelay to start and connect)
    printf("[static_sender] Sending as '%s' (%ux%u) — 10 sec of frames...\n",
           name, W, H);
    for (int f = 0; f < 300; f++) {  // 30fps x 10s = 300 frames
        if (!sender.SendImage(buf.data(), W, H))
            fprintf(stderr, "[static_sender] SendImage failed at frame %d\n", f);
        if (f % 10 == 0)
            printf("[static_sender] sent frame %d\r", f), fflush(stdout);
        Sleep(33);
    }

    // Phase 2: stop SendImage but keep the connection (IsConnected() stays true)
    printf("\n[static_sender] Stopped sending. Holding connection for 30 sec...\n");
    printf("[static_sender] SpoutRelay should keep streaming the last red frame.\n");
    fflush(stdout);
    Sleep(30000);  // wait 30 seconds

    printf("[static_sender] Exiting.\n");
    sender.ReleaseSender();
    return 0;
}
