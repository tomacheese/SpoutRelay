extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
}

#include "rtsp/rtsp_publisher_client.hpp"
#include <cstring>

struct RtspPublisherClient::Impl {
    AVFormatContext* fmt_ctx     = nullptr;
    AVStream*        video_stream = nullptr;
    int              fps         = 30;
};

RtspPublisherClient::RtspPublisherClient()
    : impl_(std::make_unique<Impl>()) {}

RtspPublisherClient::~RtspPublisherClient() { disconnect(); }

bool RtspPublisherClient::connect(const RtspConfig& config,
                                  const EncoderController::CodecInfo& codec_info,
                                  std::string& error) {
    disconnect();

    avformat_network_init();

    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_alloc_output_context2(
        &fmt_ctx, nullptr, "rtsp", config.url.c_str());

    if (ret < 0 || !fmt_ctx) {
        char buf[256]; av_strerror(ret, buf, sizeof(buf));
        error = std::string("avformat_alloc_output_context2 failed: ") + buf;
        return false;
    }

    // Add video stream
    AVStream* st = avformat_new_stream(fmt_ctx, nullptr);
    if (!st) {
        avformat_free_context(fmt_ctx);
        error = "avformat_new_stream failed";
        return false;
    }

    st->id                 = 0;
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = static_cast<AVCodecID>(codec_info.codec_id);
    st->codecpar->width      = codec_info.width;
    st->codecpar->height     = codec_info.height;
    st->codecpar->bit_rate   = codec_info.bit_rate;
    st->time_base            = {codec_info.time_base_num, codec_info.time_base_den};

    if (!codec_info.extradata.empty()) {
        st->codecpar->extradata = static_cast<uint8_t*>(
            av_mallocz(codec_info.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!st->codecpar->extradata) {
            avformat_free_context(fmt_ctx);
            error = "Failed to allocate codec extradata buffer";
            return false;
        }
        std::memcpy(st->codecpar->extradata,
                    codec_info.extradata.data(),
                    codec_info.extradata.size());
        st->codecpar->extradata_size =
            static_cast<int>(codec_info.extradata.size());
    }

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);

    // Timeouts in microseconds
    char timeout_str[32];
    snprintf(timeout_str, sizeof(timeout_str), "%d",
             config.connect_timeout_ms * 1000);
    av_dict_set(&opts, "timeout", timeout_str, 0);

    ret = avformat_write_header(fmt_ctx, &opts);
    av_dict_free(&opts);

    if (ret < 0) {
        char buf[256]; av_strerror(ret, buf, sizeof(buf));
        avformat_free_context(fmt_ctx);
        error = std::string("RTSP connect (write_header) failed: ") + buf;
        return false;
    }

    impl_->fmt_ctx      = fmt_ctx;
    impl_->video_stream = st;
    impl_->fps          = codec_info.fps;
    connected_          = true;

    // Apply per-write timeout so av_interleaved_write_frame cannot block forever
    if (impl_->fmt_ctx->pb)
        av_opt_set_int(impl_->fmt_ctx->pb, "rw_timeout",
                       static_cast<int64_t>(config.send_timeout_ms) * 1000,
                       AV_OPT_SEARCH_CHILDREN);

    return true;
}

bool RtspPublisherClient::send_packet(const EncodedPacket& pkt) {
    if (!connected_ || !impl_->fmt_ctx || !impl_->video_stream) return false;

    AVPacket* av_pkt = av_packet_alloc();
    if (!av_pkt) return false;

    int ret = av_new_packet(av_pkt, static_cast<int>(pkt.data.size()));
    if (ret < 0) { av_packet_free(&av_pkt); return false; }

    std::memcpy(av_pkt->data, pkt.data.data(), pkt.data.size());
    av_pkt->pts          = pkt.pts;
    av_pkt->dts          = pkt.dts;
    av_pkt->duration     = pkt.duration;
    av_pkt->stream_index = impl_->video_stream->index;

    if (pkt.is_key_frame) av_pkt->flags |= AV_PKT_FLAG_KEY;

    // Rescale from encoder time base to stream time base
    AVRational enc_tb = {pkt.time_base_num, pkt.time_base_den};
    av_packet_rescale_ts(av_pkt, enc_tb, impl_->video_stream->time_base);

    ret = av_interleaved_write_frame(impl_->fmt_ctx, av_pkt);
    av_packet_free(&av_pkt);

    if (ret < 0) {
        connected_ = false;
        return false;
    }

    return true;
}

void RtspPublisherClient::disconnect() {
    if (!impl_->fmt_ctx) return;

    if (connected_) {
        av_write_trailer(impl_->fmt_ctx);
    }
    // Close the underlying AVIOContext before freeing the format context to
    // avoid socket/handle leaks and interference with subsequent reconnects.
    if (impl_->fmt_ctx->pb && !(impl_->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&impl_->fmt_ctx->pb);
    }
    avformat_free_context(impl_->fmt_ctx);
    impl_->fmt_ctx      = nullptr;
    impl_->video_stream = nullptr;
    connected_          = false;
}
