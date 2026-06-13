#pragma once
#include "common/types.hpp"
#include <memory>
#include <string>

class ISpoutMonitor {
public:
    virtual ~ISpoutMonitor() = default;

    virtual bool init(std::string& error) = 0;
    virtual bool probe_sender(const std::string& name) = 0;
    virtual bool connect(const std::string& name, std::string& error) = 0;
    virtual void disconnect() = 0;
    virtual bool receive_latest_frame(FrameBuffer& buf, FrameMeta& meta, bool& is_new) = 0;
    virtual SenderInfo get_sender_info() const = 0;
    virtual bool is_connected() const = 0;

    // --- GPU ゼロコピーパス用 ---

    /// @brief SpoutDX が保持する ID3D11Device* を返す（型消去済み void*）。
    ///        EncoderController に渡し、FFmpeg の AVHWDeviceContext を共有する。
    ///        init() 前は nullptr を返す。
    virtual void* gpu_device() = 0;

    /// @brief GPU テクスチャモードを有効/無効にする。
    ///        有効時は receive_latest_frame() で ReceiveImage() の代わりに
    ///        ReceiveTexture() を呼び出し、buf.gpu_texture に内部テクスチャ
    ///        ポインタをセットする（buf.data は空のまま）。
    ///        無効時は従来の CPU コピーパスを使用する。
    virtual void set_gpu_mode(bool enabled) = 0;
};

class SpoutMonitor : public ISpoutMonitor {
public:
    SpoutMonitor();
    ~SpoutMonitor() override;

    bool init(std::string& error) override;
    bool probe_sender(const std::string& name) override;
    bool connect(const std::string& name, std::string& error) override;
    void disconnect() override;
    bool receive_latest_frame(FrameBuffer& buf, FrameMeta& meta, bool& is_new) override;
    SenderInfo get_sender_info() const override;
    bool is_connected() const override;
    void* gpu_device() override;
    void set_gpu_mode(bool enabled) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
