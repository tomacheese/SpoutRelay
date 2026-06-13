#include <windows.h>
#include <d3d11.h>
#include <mutex>
#include "spout/spout_monitor.hpp"
#include "common/time_utils.hpp"
#include "SpoutDX.h"

struct SpoutMonitor::Impl {
    spoutDX    receiver;
    SenderInfo current_info;
    bool       connected  = false;
    uint64_t   sequence   = 0;
    bool       gpu_mode   = false;  ///< true: ReceiveTexture() パス, false: ReceiveImage() パス
    std::mutex mutex_;
};

SpoutMonitor::SpoutMonitor()  : impl_(std::make_unique<Impl>()) {}
SpoutMonitor::~SpoutMonitor() { disconnect(); }

bool SpoutMonitor::init(std::string& error) {
    impl_->receiver.SetAdapterAuto(true); // auto-switch to sender's GPU adapter
    if (!impl_->receiver.OpenDirectX11()) {
        error = "Failed to initialise DirectX 11 for Spout receiver";
        return false;
    }

    // D3D11 Immediate Context のスレッド安全性を有効化する (ScreenRelay PR #7 由来)。
    // EncoderController が別スレッドから CopySubresourceRegion を呼び出すため、
    // ID3D10Multithread インターフェースで排他制御を有効にする。
    ID3D11Device* device = impl_->receiver.GetDX11Device();
    if (device) {
        ID3D10Multithread* mt = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D10Multithread),
                                             reinterpret_cast<void**>(&mt)))) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
        }
    }

    return true;
}

bool SpoutMonitor::probe_sender(const std::string& name) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    unsigned int w = 0, h = 0;
    HANDLE handle  = nullptr;
    DWORD  format  = 0;
    return impl_->receiver.GetSenderInfo(name.c_str(), w, h, handle, format);
}

bool SpoutMonitor::connect(const std::string& name, std::string& error) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->receiver.SetReceiverName(name.c_str());
    impl_->connected = true;
    impl_->current_info.name  = name;
    impl_->current_info.valid = false; // dimensions not yet known
    return true;
}

void SpoutMonitor::disconnect() {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (impl_->connected) {
        impl_->receiver.ReleaseReceiver();
        impl_->connected = false;
        impl_->current_info = SenderInfo{};
    }
}

bool SpoutMonitor::receive_latest_frame(FrameBuffer& buf,
                                        FrameMeta& meta,
                                        bool& is_new) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->connected) return false;

    unsigned int w = impl_->current_info.width;
    unsigned int h = impl_->current_info.height;

    // If dimensions are unknown, query them from the sender info first
    if (w == 0 || h == 0) {
        HANDLE handle = nullptr;
        DWORD  format = 0;
        if (!impl_->receiver.GetSenderInfo(
                impl_->current_info.name.c_str(), w, h, handle, format))
            return false;
        if (w == 0 || h == 0) return false;
        impl_->current_info.width  = w;
        impl_->current_info.height = h;
        impl_->current_info.valid  = true;
    }

    bool ok = false;

    if (impl_->gpu_mode) {
        // GPU ゼロコピーパス: ReceiveTexture() で内部テクスチャを更新し、
        // ポインタだけを FrameBuffer に格納する（CPU メモリコピーなし）
        ID3D11Texture2D* tex = nullptr;
        ok = impl_->receiver.ReceiveTexture(&tex);

        impl_->connected = impl_->receiver.IsConnected();
        if (!ok) return false;

        // 寸法変更 / 初回接続更新イベントの処理
        if (impl_->receiver.IsUpdated()) {
            unsigned int nw = impl_->receiver.GetSenderWidth();
            unsigned int nh = impl_->receiver.GetSenderHeight();
            impl_->current_info.width  = nw ? nw : w;
            impl_->current_info.height = nh ? nh : h;
            impl_->current_info.name   = impl_->receiver.GetSenderName();
            impl_->current_info.fps    = static_cast<float>(impl_->receiver.GetSenderFps());
            if (nw && nh && (nw != w || nh != h)) { w = nw; h = nh; }
            is_new = false;  // 更新イベント: 次回呼び出しで実データ取得
        } else {
            is_new = impl_->receiver.IsFrameNew();
        }

        buf.data.clear();             // GPU パスでは CPU バッファ不要
        buf.gpu_texture = tex;        // EncoderController が CopySubresourceRegion に使用
        buf.width  = w;
        buf.height = h;
        buf.format = PixelFormat::RGBA;
    } else {
        // CPU パス（従来動作）
        size_t required = static_cast<size_t>(w) * h * 4;
        if (buf.data.size() != required) buf.data.resize(required);

        // bRGB=false, bInvert=false → RGBA output (raw DX11 texture order)
        ok = impl_->receiver.ReceiveImage(buf.data.data(), w, h, false, false);

        impl_->connected = impl_->receiver.IsConnected();
        if (!ok) return false;

        // 寸法変更 / 初回接続更新イベントの処理
        if (impl_->receiver.IsUpdated()) {
            unsigned int nw = impl_->receiver.GetSenderWidth();
            unsigned int nh = impl_->receiver.GetSenderHeight();
            impl_->current_info.width  = nw ? nw : w;
            impl_->current_info.height = nh ? nh : h;
            impl_->current_info.name   = impl_->receiver.GetSenderName();
            impl_->current_info.fps    = static_cast<float>(impl_->receiver.GetSenderFps());

            if (nw && nh && (nw != w || nh != h)) {
                required = static_cast<size_t>(nw) * nh * 4;
                buf.data.resize(required);
                // ReceiveImage returns true on updated event; actual pixel copy happens
                // next call when the buffer matches the new dimensions
                w = nw; h = nh;
            }
            // First-connection update: treat as frame-not-yet-new, caller will retry
            is_new = false;
        } else {
            is_new = impl_->receiver.IsFrameNew();
        }

        buf.gpu_texture = nullptr;  // CPU パスでは GPU テクスチャ不要
        buf.width  = w;
        buf.height = h;
        buf.format = PixelFormat::RGBA;
    }

    meta.width        = w;
    meta.height       = h;
    meta.sequence     = ++impl_->sequence;
    meta.timestamp_us = time_utils::now_us();

    return true;
}

SenderInfo SpoutMonitor::get_sender_info() const {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->current_info;
}

bool SpoutMonitor::is_connected() const {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->connected && impl_->receiver.IsConnected();
}

void* SpoutMonitor::gpu_device() {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return static_cast<void*>(impl_->receiver.GetDX11Device());
}

void SpoutMonitor::set_gpu_mode(bool enabled) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->gpu_mode = enabled;
}
