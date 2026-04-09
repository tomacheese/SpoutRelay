// Simple Spout test sender — generates a color-cycling 640x360 BGRA frame
// Usage: spout_test_sender.exe [sender_name]
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>
#include <cmath>
#include "SpoutDX.h"

int main(int argc, char* argv[]) {
    const char* name = (argc > 1) ? argv[1] : "TestSpoutSender";
    const unsigned int W = 640, H = 360;

    spoutDX sender;
    if (!sender.OpenDirectX11()) {
        fprintf(stderr, "[sender] Failed to open DirectX11\n");
        return 1;
    }

    sender.SetSenderName(name);
    printf("[sender] Sending as '%s' (%ux%u)...\n", name, W, H);
    fflush(stdout);

    std::vector<unsigned char> buf(W * H * 4);
    int frame = 0;

    while (true) {
        // Color cycle: hue shifts each frame
        float hue = fmod(frame * 2.0f, 360.0f);
        float h = hue / 60.0f;
        int i = (int)h;
        float f = h - i;
        unsigned char r, g, b;
        // Simple HSV->RGB with S=1, V=200
        unsigned char v = 200;
        unsigned char p = 0;
        unsigned char q = (unsigned char)(v * (1.0f - f));
        unsigned char t = (unsigned char)(v * f);
        switch (i % 6) {
            case 0: r=v; g=t; b=p; break;
            case 1: r=q; g=v; b=p; break;
            case 2: r=p; g=v; b=t; break;
            case 3: r=p; g=q; b=v; break;
            case 4: r=t; g=p; b=v; break;
            default: r=v; g=p; b=q; break;
        }

        // Fill frame BGRA
        for (unsigned int y = 0; y < H; y++) {
            for (unsigned int x = 0; x < W; x++) {
                unsigned int idx = (y * W + x) * 4;
                buf[idx + 0] = b;
                buf[idx + 1] = g;
                buf[idx + 2] = r;
                buf[idx + 3] = 255;
            }
        }
        // Draw a gradient bar in top-left corner as frame counter indicator
        for (unsigned int y = 0; y < 20 && y < H; y++)
            for (unsigned int x = 0; x < 100 && x < W; x++) {
                unsigned int idx = (y * W + x) * 4;
                unsigned char lum = (unsigned char)((x * 2) & 0xFF);
                buf[idx+0] = buf[idx+1] = buf[idx+2] = lum;
                buf[idx+3] = 255;
            }

        if (!sender.SendImage(buf.data(), W, H)) {
            fprintf(stderr, "[sender] SendImage failed at frame %d\n", frame);
        }

        if (frame % 30 == 0)
            printf("[sender] frame %d\r", frame), fflush(stdout);

        frame++;
        Sleep(33); // ~30 fps
    }

    sender.ReleaseSender();
    return 0;
}
