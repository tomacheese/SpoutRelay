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

    bool init(const EncoderConfig& config,
              uint32_t width, uint32_t height,
              std::string& error);

    /**
     * @param content_changed 直前の encode() 呼び出しからピクセル内容が変化している場合は true。
     *                        false を渡すと RGBA→YUV の sws_scale 変換を省略し、
     *                        直前の変換済み YUV フレームを再利用する。
     *                        静止画面のフリーズフレーム再送時に CPU 負荷を大幅に削減できる。
     */
    bool encode(const FrameBuffer& frame,
                const FrameMeta& meta,
                std::vector<EncodedPacket>& out_packets,
                bool content_changed = true);

    bool flush(std::vector<EncodedPacket>& out_packets);

    void reset();

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    int      fps()    const;
    int      bitrate_kbps() const;

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
        // 色空間メタデータ
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
