#pragma once
#include "common/types.hpp"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

extern "C" {
#include <libavutil/pixfmt.h>
}

struct EncodedPacket {
    std::vector<uint8_t> data;
    int64_t pts      = 0;
    int64_t dts      = 0;
    int64_t duration = 0;
    bool is_key_frame = false;
    int  time_base_num = 1;
    int  time_base_den = 30;
};

class EncoderController {
public:
    EncoderController();
    ~EncoderController();

    /// @param d3d_device          Type-erased ID3D11Device* from SpoutMonitor::gpu_device().
    ///                            If non-null and the codec is NVENC/AMF/QSV, the GPU
    ///                            zero-copy path is attempted. If GPU init fails it falls
    ///                            back to the CPU path automatically (not an error).
    ///                            nullptr always uses the CPU path.
    /// @param sender_dxgi_format  Return value of SpoutMonitor::get_sender_dxgi_format().
    ///                            Matches the GPU pool texture format to the sender's actual
    ///                            format to avoid CopySubresourceRegion's silent failure.
    ///                            0 assumes BGRA (DXGI_FORMAT_B8G8R8A8_UNORM = 87).
    bool init(const EncoderConfig& config,
              uint32_t width, uint32_t height,
              std::string& error,
              void* d3d_device = nullptr,
              uint32_t sender_dxgi_format = 0);

    /**
     * @param content_changed true if the pixels changed since the previous encode() call.
     *                        If false, the RGBA->YUV sws_scale conversion is skipped and
     *                        the previous YUV frame is reused.
     *                        Greatly reduces CPU load when resending a static freeze frame.
     */
    bool encode(const FrameBuffer& frame,
                const FrameMeta& meta,
                std::vector<EncodedPacket>& out_packets,
                bool content_changed = true);

    bool flush(std::vector<EncodedPacket>& out_packets);

    /**
     * @brief Requests that the next frame be emitted as an IDR (keyframe)
     *
     * Call after an RTSP reconnect so the receiver can show full video right after
     * connecting. The IDR is inserted into the GOP without resetting the encoder,
     * so PTS continuity is kept.
     * The flag is cleared on the next encode() call once consumed.
     */
    void request_next_idr();

    void reset();

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    int      fps()    const;
    int      bitrate_kbps() const;

    /// @brief Returns whether the GPU zero-copy path is active.
    ///        If true, the caller should call SpoutMonitor::set_gpu_mode(true) to switch to
    ///        the path that uses FrameBuffer.gpu_texture.
    bool gpu_path_active() const;

    // Codec parameters for RTSP stream initialisation
    // Caller receives borrowed pointer valid until reset()/destructor
    struct CodecInfo {
        int codec_id    = 0; // AVCodecID value
        int width       = 0;
        int height      = 0;
        int fps         = 30;
        int bit_rate    = 0;
        int time_base_num = 1;
        int time_base_den = 30;
        std::vector<uint8_t> extradata; // SPS/PPS
        // Color space metadata
        int color_range      = AVCOL_RANGE_UNSPECIFIED;
        int colorspace       = AVCOL_SPC_UNSPECIFIED;
        int color_primaries  = AVCOL_PRI_UNSPECIFIED;
        int color_trc        = AVCOL_TRC_UNSPECIFIED;
    };

    CodecInfo get_codec_info() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    EncoderConfig config_;
};
