#include <windows.h>
#include <mutex>
#include "spout/spout_monitor.hpp"
#include "common/time_utils.hpp"
#include "SpoutDX.h"

struct SpoutMonitor::Impl {
    spoutDX  receiver;
    SenderInfo current_info;
    bool     connected = false;
    uint64_t sequence  = 0;
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

    size_t required = static_cast<size_t>(w) * h * 4;
    if (buf.data.size() != required) buf.data.resize(required);

    // bRGB=false, bInvert=false → RGBA output (raw DX11 texture order)
    bool ok = impl_->receiver.ReceiveImage(buf.data.data(), w, h, false, false);

    impl_->connected = impl_->receiver.IsConnected();

    if (!ok) return false;

    // Handle dimension changes / first-connection update event
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

    buf.width  = w;
    buf.height = h;
    buf.format = PixelFormat::RGBA;

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
