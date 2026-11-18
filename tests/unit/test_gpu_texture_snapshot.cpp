#include "spout/gpu_texture_snapshot.hpp"
#include "common/types.hpp"
#include "test_utils.hpp"
#include <array>
#include <utility>

static void verify_pixels(ID3D11Device* device, ID3D11DeviceContext* context,
                          const FrameBuffer& frame, uint32_t expected) {
    auto* texture = static_cast<ID3D11Texture2D*>(frame.gpu_texture.get());
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    VERIFY(desc.Width == frame.width && desc.Height == frame.height);
    VERIFY(desc.Format == (frame.format == PixelFormat::RGBA
        ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    VERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
    context->CopyResource(staging, texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    VERIFY(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)));
    for (UINT y = 0; y < desc.Height; ++y) {
        auto* row = reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
        for (UINT x = 0; x < desc.Width; ++x) VERIFY(row[x] == expected);
    }
    context->Unmap(staging, 0);
    staging->Release();
}

int run_gpu_texture_snapshot_tests() {
    printf("=== GPU Texture Snapshot Tests ===\n");
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    VERIFY_MSG(SUCCEEDED(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context)),
        "D3D11 software device should be available without GPU hardware");

    VERIFY(!snapshot_gpu_texture(nullptr, context, nullptr));
    VERIFY(!snapshot_gpu_texture(device, nullptr, nullptr));
    VERIFY(!snapshot_gpu_texture(device, context, nullptr));

    std::vector<FrameBuffer> retained;
    for (auto format : {PixelFormat::BGRA, PixelFormat::RGBA}) {
        for (UINT size : {2u, 4u}) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = size;
            desc.Height = size;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = format == PixelFormat::RGBA
                ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            ID3D11Texture2D* source = nullptr;
            VERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &source)));

            FrameBuffer first;
            first.width = first.height = size;
            first.format = format;
            std::array<uint32_t, 16> pixels;
            pixels.fill(0xff123456);
            context->UpdateSubresource(source, 0, nullptr, pixels.data(), size * 4, 0);
            first.gpu_texture = snapshot_gpu_texture(device, context, source);
            VERIFY(first.gpu_texture && first.gpu_texture.get() != source);
            auto freeze = first;
            auto queued = std::move(first);
            VERIFY(!first.gpu_texture);

            pixels.fill(0xffabcdef);
            context->UpdateSubresource(source, 0, nullptr, pixels.data(), size * 4, 0);
            FrameBuffer second;
            second.width = second.height = size;
            second.format = format;
            second.gpu_texture = snapshot_gpu_texture(device, context, source);
            VERIFY(second.gpu_texture && second.gpu_texture != queued.gpu_texture);
            source->Release();

            verify_pixels(device, context, queued, 0xff123456);
            verify_pixels(device, context, second, 0xffabcdef);
            std::weak_ptr<void> lifetime = queued.gpu_texture;
            queued = {};
            VERIFY(!lifetime.expired());
            verify_pixels(device, context, freeze, 0xff123456);
            freeze = {};
            VERIFY(lifetime.expired());
            retained.push_back(std::move(second));
        }
    }
    for (const auto& frame : retained) verify_pixels(device, context, frame, 0xffabcdef);
    context->Release();
    device->Release();
    printf("[PASS] independent BGRA/RGBA snapshots survive source overwrite, release and resize\n");
    return 0;
}
