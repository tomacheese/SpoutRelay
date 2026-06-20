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
    mutable std::mutex mutex_;
};

SpoutMonitor::SpoutMonitor()  : impl_(std::make_unique<Impl>()) {}
SpoutMonitor::~SpoutMonitor() { disconnect(); }

/// @brief D3D11 Immediate Context のマルチスレッド保護を有効にする。
///        init() と reinit_device() の両方から呼び出す共通ヘルパー。
///        init() はシングルスレッド起動中にロックなしで呼び出す。
///        reinit_device() は impl_->mutex_ 保持済みの状態で呼び出す。
static void setup_device_mt_protection(spoutDX& receiver) {
    // D3D11 Immediate Context のスレッド安全性を有効化する (ScreenRelay PR #7 由来)。
    // EncoderController が別スレッドから CopySubresourceRegion を呼び出すため、
    // ID3D10Multithread インターフェースで排他制御を有効にする。
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
    impl_->receiver.SetAdapterAuto(true); // auto-switch to sender's GPU adapter
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
        // デバイスロスト時は ReceiveTexture() を呼ばずに早期リターンする。
        // 死んだデバイスへの SpoutDX 呼び出しはアクセス違反を引き起こす可能性がある。
        {
            ID3D11Device* dev = impl_->receiver.GetDX11Device();
            if (dev && dev->GetDeviceRemovedReason() != S_OK)
                return false;
        }

        // GPU ゼロコピーパス:
        //   ReceiveTexture() (引数なし) で SpoutDX 内部テクスチャ (m_pTexture) を
        //   更新し、GetSenderTexture() でそのポインタを取得して FrameBuffer に
        //   格納する。CPU メモリへの読み戻しは発生しない。
        //   ※ ReceiveTexture(ID3D11Texture2D**) は呼び元で確保済みテクスチャを
        //      渡す API であり、*ppTexture が null だと false を返すため使用しない。
        ok = impl_->receiver.ReceiveTexture();

        impl_->connected = impl_->receiver.IsConnected();
        if (!ok) return false;

        // 寸法変更 / 初回接続更新イベントの処理
        //
        // ReceiveTexture() (引数なし) は内部で m_bUpdated をリセットするため
        // IsUpdated() は常に false を返す。そのため IsUpdated() に加えて
        // GetSenderWidth/Height() を直接比較して寸法変更を確実に検出する。
        // （ReceiveImage() や ReceiveTexture(ppTexture) は IsUpdated() が機能するが
        //   ReceiveTexture() 無引数版のみ内部処理でフラグを消費してしまう仕様）
        {
            unsigned int nw = impl_->receiver.GetSenderWidth();
            unsigned int nh = impl_->receiver.GetSenderHeight();
            bool updated = impl_->receiver.IsUpdated();  // 初回接続時などは true
            bool dim_changed = (nw && nh && (nw != w || nh != h));

            if (updated || dim_changed) {
                impl_->current_info.width  = nw ? nw : w;
                impl_->current_info.height = nh ? nh : h;
                impl_->current_info.name   = impl_->receiver.GetSenderName();
                impl_->current_info.fps    = static_cast<float>(impl_->receiver.GetSenderFps());
                if (nw && nh && (nw != w || nh != h)) { w = nw; h = nh; }
                is_new = false;  // 更新イベント: 次回呼び出しで実データ取得
            } else {
                is_new = impl_->receiver.IsFrameNew();
            }
        }

        // GetSenderTexture() は SpoutDX 内部テクスチャのポインタを返す。
        // このポインタは次回 ReceiveTexture() 呼び出しまで有効。
        // フォーマットはセンダー依存: BGRA (87) が多いが Unity/VRChat 等は RGBA (28) を使う。
        // EncoderController::init() が get_sender_dxgi_format() で取得した値に基づいて
        // GPU プールのフォーマットを合わせているため、センダーに合わせた値を設定する。
        ID3D11Texture2D* tex = impl_->receiver.GetSenderTexture();

        buf.data.clear();             // GPU パスでは CPU バッファ不要
        buf.gpu_texture = tex;        // EncoderController が CopySubresourceRegion に使用
        buf.width  = w;
        buf.height = h;
        // EncoderController のフォーマット選択ロジック (encoder_controller.cpp) と方向を揃える:
        //   DXGI_FORMAT_R8G8B8A8_UNORM (RGBA) → PixelFormat::RGBA
        //   それ以外 (DXGI_FORMAT_B8G8R8A8_UNORM=BGRA および未知フォーマット) → PixelFormat::BGRA
        // ※ 未知フォーマットは BGRA 扱いとすることで encoder 側デフォルトと一致させる。
        const DWORD dxgi_fmt = impl_->receiver.GetSenderFormat();
        buf.format = (dxgi_fmt == DXGI_FORMAT_R8G8B8A8_UNORM) ? PixelFormat::RGBA : PixelFormat::BGRA;
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

uint32_t SpoutMonitor::get_sender_dxgi_format() const {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    // GetSenderFormat() は DXGI_FORMAT を DWORD で返す。
    // connect() は impl_->connected=true にするだけで実接続前でも true になり得るため、
    // is_connected() と同様に receiver.IsConnected() も確認することで、
    // 未接続状態での GetSenderFormat() のデフォルト値/前回値の混入を防ぐ。
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
    // 既存の受信セッションとデバイスを破棄する
    impl_->receiver.ReleaseReceiver();
    impl_->receiver.CloseDirectX11();
    impl_->connected      = false;
    impl_->current_info   = SenderInfo{};
    impl_->gpu_mode       = false;  // CPU パスへ戻し、再初期化後に Supervisor が再設定する

    // D3D11 デバイスを再作成する
    if (!impl_->receiver.OpenDirectX11()) {
        error = "Failed to re-create DirectX 11 device after device loss";
        return false;
    }
    setup_device_mt_protection(impl_->receiver);
    return true;
}
