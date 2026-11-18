#pragma once
#include <d3d11.h>
#include <memory>

inline std::shared_ptr<void> snapshot_gpu_texture(ID3D11Device* device,
                                                 ID3D11DeviceContext* context,
                                                 ID3D11Texture2D* source) {
    if (!device || !context || !source) return {};

    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    ID3D11Texture2D* texture = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) return {};

    std::shared_ptr<void> snapshot(texture, [](void* ptr) {
        static_cast<ID3D11Texture2D*>(ptr)->Release();
    });
    // Use the receiver's immediate context so this copy precedes its next overwrite.
    context->CopyResource(texture, source);
    return snapshot;
}
