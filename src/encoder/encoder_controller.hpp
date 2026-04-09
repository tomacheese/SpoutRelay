#pragma once
#include "common/types.hpp"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

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

    bool encode(const FrameBuffer& frame,
                const FrameMeta& meta,
                std::vector<EncodedPacket>& out_packets);

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
    };

    CodecInfo get_codec_info() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    EncoderConfig config_;
};
