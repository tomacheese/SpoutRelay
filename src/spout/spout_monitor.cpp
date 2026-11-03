#include <windows.h>
#include <d3d11.h>
#include <mutex>
#include "spout/spout_monitor.hpp"
#include "common/time_utils.hpp"
#include "SpoutDX.h"

struct SpoutMonitor::Impl {
    spoutDX    receiver;
    SenderInfo current_info;
    /// Set once connect() is called and cleared by disconnect().
    /// Not updated in receive_latest_frame(); the sender's real connection state is
    /// checked via receiver.IsConnected(), and is_connected() ANDs both.
    bool       connected  = false;
    uint64_t   sequence   = 0;
    bool       gpu_mode   = false;  ///< true: ReceiveTexture() path, false: ReceiveImage() path
    mutable std::mutex mutex_;
};

SpoutMonitor::SpoutMonitor()  : impl_(std::make_unique<Impl>()) {}
SpoutMonitor::~SpoutMonitor() { disconnect(); }

/// @brief Enables multithread protection on the D3D11 immediate context.
///        Shared helper called from both init() and reinit_device().
///        init() calls it lock-free during single-threaded startup;
///        reinit_device() calls it while holding impl_->mutex_.
static void setup_device_mt_protection(spoutDX& receiver) {
    // Enable thread safety on the D3D11 immediate context.
    // EncoderController calls CopySubresourceRegion from another thread, so enable
    // locking via the ID3D10Multithread interface.
    ID3D11Device* device = receiver.GetDX11Device();
    if (device) {
        ID3D10Multithread* mt = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D10Multithread),
                                             reinterpret_cast<void**>(&mt)))) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
        }
    }
}

bool SpoutMonitor::init(std::string& error) {
    // Disable automatic adapter switching. When enabled, a changed shared handle makes
    // CheckSenderTexture() recreate the D3D11 device synchronously while holding
    // impl_->mutex_, which can freeze.
    impl_->receiver.SetAdapterAuto(false);
    if (!impl_->receiver.OpenDirectX11()) {
        error = "Failed to initialise DirectX 11 for Spout receiver";
        return false;
    }

    setup_device_mt_protection(impl_->receiver);
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
        // On device loss, return early without calling ReceiveTexture();
        // SpoutDX calls on a dead device can cause an access violation.
        {
            ID3D11Device* dev = impl_->receiver.GetDX11Device();
            if (dev && dev->GetDeviceRemovedReason() != S_OK)
                return false;
        }

        // GPU zero-copy path:
        //   ReceiveTexture() (no arguments) updates SpoutDX's internal texture
        //   (m_pTexture); GetSenderTexture() returns its pointer, which is stored in
        //   the FrameBuffer. No readback to CPU memory occurs.
        //   Note: ReceiveTexture(ID3D11Texture2D**) takes a caller-allocated texture and
        //   returns false if *ppTexture is null, so it is not used.
        ok = impl_->receiver.ReceiveTexture();

        // Do not update impl_->connected here.
        // Setting it from receiver.IsConnected() would make it false while the sender
        // is temporarily disconnected, and the guard at the top of receive_latest_frame()
        // would keep rejecting calls after it returns. SpoutDX with SetReceiverName()
        // detects the sender's return by calling ReceiveTexture() again, so connected
        // is left unchanged.
        // Sender loss is detected by the Supervisor via a stalled last_source_alive_ms().
        if (!ok) return false;

        // Handle dimension changes / the initial connection update event
        //
        // ReceiveTexture() (no arguments) resets m_bUpdated internally, so IsUpdated()
        // always returns false. Compare GetSenderWidth/Height() directly, in addition to
        // IsUpdated(), to reliably detect dimension changes.
        // (IsUpdated() works with ReceiveImage() and ReceiveTexture(ppTexture); only the
        // no-argument ReceiveTexture() consumes the flag internally.)
        {
            unsigned int nw = impl_->receiver.GetSenderWidth();
            unsigned int nh = impl_->receiver.GetSenderHeight();
            bool updated = impl_->receiver.IsUpdated();  // true e.g. on first connection
            bool dim_changed = (nw && nh && (nw != w || nh != h));

            if (updated || dim_changed) {
                impl_->current_info.width  = nw ? nw : w;
                impl_->current_info.height = nh ? nh : h;
                impl_->current_info.name   = impl_->receiver.GetSenderName();
                impl_->current_info.fps    = static_cast<float>(impl_->receiver.GetSenderFps());
                if (nw && nh && (nw != w || nh != h)) { w = nw; h = nh; }
                is_new = false;  // update event: fetch real data on the next call
            } else {
                is_new = impl_->receiver.IsFrameNew();
            }
        }

        // GetSenderTexture() returns a pointer to SpoutDX's internal texture,
        // valid until the next ReceiveTexture() call.
        // The format depends on the sender: mostly BGRA (87), but Unity/VRChat etc. use RGBA (28).
        // EncoderController::init() sizes the GPU pool from get_sender_dxgi_format(),
        // so the value set here follows the sender.
        ID3D11Texture2D* tex = impl_->receiver.GetSenderTexture();

        buf.data.clear();             // no CPU buffer needed on the GPU path
        buf.gpu_texture = tex;        // used by EncoderController for CopySubresourceRegion
        buf.width  = w;
        buf.height = h;
        // Mirrors the encoder's format selection logic (encoder_controller.cpp):
        //   DXGI_FORMAT_R8G8B8A8_UNORM (RGBA) -> PixelFormat::RGBA
        //   anything else (DXGI_FORMAT_B8G8R8A8_UNORM=BGRA and unknown formats) -> PixelFormat::BGRA
        // Unknown formats are treated as BGRA to match the encoder default.
        const DWORD dxgi_fmt = impl_->receiver.GetSenderFormat();
        buf.format = (dxgi_fmt == DXGI_FORMAT_R8G8B8A8_UNORM) ? PixelFormat::RGBA : PixelFormat::BGRA;
    } else {
        // CPU path (legacy behavior)
        size_t required = static_cast<size_t>(w) * h * 4;
        if (buf.data.size() != required) buf.data.resize(required);

        // bRGB=false, bInvert=false → RGBA output (raw DX11 texture order)
        ok = impl_->receiver.ReceiveImage(buf.data.data(), w, h, false, false);

        // Do not update impl_->connected here (same reason as the GPU path).
        // ReceiveImage() reconnects automatically when called again after the sender
        // disconnects and returns, so connected must not be cleared.
        if (!ok) return false;

        // Handle dimension changes / the initial connection update event
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

        buf.gpu_texture = nullptr;  // no GPU texture on the CPU path
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
    // Known limitation: with SetAdapterAuto(false), if a shared handle from a
    // mismatched adapter cannot be opened, the SDK's IsConnected() may stay true
    // (harmless since no caller relies on it).
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

uint32_t SpoutMonitor::get_sender_dxgi_format() const {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    // GetSenderFormat() returns a DXGI_FORMAT as a DWORD.
    // connect() only sets impl_->connected=true, which can be true before a real
    // connection, so also check receiver.IsConnected() as is_connected() does.
    // This avoids leaking GetSenderFormat()'s default/stale value while disconnected.
    if (!impl_->connected || !impl_->receiver.IsConnected()) return 0;
    return static_cast<uint32_t>(impl_->receiver.GetSenderFormat());
}

bool SpoutMonitor::is_device_removed() const {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    ID3D11Device* dev = impl_->receiver.GetDX11Device();
    if (!dev) return false;
    return dev->GetDeviceRemovedReason() != S_OK;
}

bool SpoutMonitor::reinit_device(std::string& error) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    // Discard the existing receive session and device
    impl_->receiver.ReleaseReceiver();
    impl_->receiver.CloseDirectX11();
    impl_->connected      = false;
    impl_->current_info   = SenderInfo{};
    impl_->gpu_mode       = false;  // back to the CPU path; the Supervisor re-enables it after reinit

    if (!impl_->receiver.OpenDirectX11()) {
        error = "Failed to re-create DirectX 11 device after device loss";
        return false;
    }
    setup_device_mt_protection(impl_->receiver);
    return true;
}
