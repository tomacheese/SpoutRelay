#pragma once
#include "common/types.hpp"
#include "encoder/encoder_controller.hpp"
#include <string>
#include <memory>

class RtspPublisherClient {
public:
    RtspPublisherClient();
    ~RtspPublisherClient();

    bool connect(const RtspConfig& config,
                 const EncoderController::CodecInfo& codec_info,
                 std::string& error);

    bool send_packet(const EncodedPacket& pkt);

    void disconnect();

    bool is_connected() const { return connected_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool connected_ = false;
};
