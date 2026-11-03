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

    // --- For the GPU zero-copy path ---

    /// @brief Returns the ID3D11Device* held by SpoutDX (type-erased void*).
    ///        Passed to EncoderController so it shares FFmpeg's AVHWDeviceContext.
    ///        Returns nullptr before init().
    virtual void* gpu_device() = 0;

    /// @brief Enables/disables GPU texture mode.
    ///        When enabled, receive_latest_frame() calls ReceiveTexture() instead of
    ///        ReceiveImage() and sets the internal texture pointer in buf.gpu_texture
    ///        (buf.data stays empty).
    ///        When disabled, the legacy CPU copy path is used.
    virtual void set_gpu_mode(bool enabled) = 0;

    /// @brief Returns the DXGI format value of the connected Spout sender's texture.
    ///        Returns 0 before or without a connection.
    ///        Passed to EncoderController::init() to match the GPU pool format to the
    ///        sender's, preventing CopySubresourceRegion's silent failure.
    ///        Typical values: 28 = DXGI_FORMAT_R8G8B8A8_UNORM (RGBA, Unity/VRChat etc.),
    ///                        87 = DXGI_FORMAT_B8G8R8A8_UNORM (BGRA, default).
    virtual uint32_t get_sender_dxgi_format() const = 0;

    // --- For GPU device-lost recovery ---

    /// @brief Checks whether the D3D11 device is in the lost state.
    ///        Used to detect DXGI_ERROR_DEVICE_REMOVED after a GPU TDR.
    ///        Returns false before init() or when no device is held.
    virtual bool is_device_removed() const = 0;

    /// @brief Recreates the D3D11 device (for GPU TDR recovery).
    ///        Rebuilds the device via CloseDirectX11() -> OpenDirectX11() and
    ///        re-applies multithread protection.
    ///        Returns true on success; on failure stores a message in error and returns false.
    virtual bool reinit_device(std::string& error) = 0;
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
    uint32_t get_sender_dxgi_format() const override;
    bool is_device_removed() const override;
    bool reinit_device(std::string& error) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
