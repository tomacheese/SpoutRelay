extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
}

#include <windows.h>
#include <d3d11.h>

#include "encoder/encoder_controller.hpp"
#include <cstring>
#include <stdexcept>

struct EncoderController::Impl {
    const AVCodec*   codec      = nullptr;
    AVCodecContext*  codec_ctx  = nullptr;
    SwsContext*      sws_ctx    = nullptr;
    AVFrame*         yuv_frame  = nullptr;
    AVPacket*        pkt        = nullptr;
    int64_t          frame_count = 0;
    std::string      codec_name;

    bool             force_next_idr = false;  ///< force the next frame out as an IDR

    // --- GPU zero-copy path ---
    bool             gpu_path     = false;
    AVBufferRef*     hw_device_ctx = nullptr;
    AVBufferRef*     hw_frames_ctx = nullptr;
    AVFrame*         last_hw_frame = nullptr; ///< HW frame reused when content_changed=false
    ID3D11DeviceContext* d3d_ctx   = nullptr; ///< for CopySubresourceRegion (borrowed)
    ID3D11Device*    d3d_device    = nullptr; ///< for device-lost detection (AddRef held)
};

EncoderController::EncoderController()
    : impl_(std::make_unique<Impl>()) {}

EncoderController::~EncoderController() { reset(); }

static AVPixelFormat pick_pix_fmt(const AVCodec* codec) {
    // Use the new API if available (FFmpeg 7+)
    const AVPixelFormat* fmts = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 0, 0)
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT,
                                     0, (const void**)&fmts, nullptr) < 0)
        fmts = nullptr;
#else
    fmts = codec->pix_fmts;
#endif
    if (!fmts) return AV_PIX_FMT_YUV420P;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_YUV420P;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
    return fmts[0];
}

// ---------------------------------------------------------------------------
// GPU zero-copy path initialization helper.
// On success returns true and sets codec_ctx->hw_frames_ctx.
// On failure releases acquired resources and returns false (caller falls back to CPU).
// ---------------------------------------------------------------------------
static bool try_init_gpu_path(AVCodecContext* ctx,
                              void* raw_device,
                              uint32_t width, uint32_t height,
                              AVBufferRef** out_hw_device_ctx,
                              AVBufferRef** out_hw_frames_ctx,
                              ID3D11DeviceContext** out_d3d_ctx,
                              AVPixelFormat sw_fmt = AV_PIX_FMT_BGRA)
{
    auto* d3d11dev = static_cast<ID3D11Device*>(raw_device);
    if (!d3d11dev) return false;

    // 1. Wrap the existing D3D11 device in an FFmpeg HW device context
    AVBufferRef* hw_dev_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_dev_ctx) return false;

    auto* dev_ctx  = reinterpret_cast<AVHWDeviceContext*>(hw_dev_ctx->data);
    auto* d3d11ctx = static_cast<AVD3D11VADeviceContext*>(dev_ctx->hwctx);

    d3d11dev->AddRef();               // FFmpeg will Release it
    d3d11ctx->device = d3d11dev;

    // Get and save the D3D11 device context (for CopySubresourceRegion)
    d3d11dev->GetImmediateContext(out_d3d_ctx);

    if (av_hwdevice_ctx_init(hw_dev_ctx) < 0) {
        av_buffer_unref(&hw_dev_ctx);
        if (*out_d3d_ctx) { (*out_d3d_ctx)->Release(); *out_d3d_ctx = nullptr; }
        return false;
    }

    // 2. Build the HW frame context
    //    format=D3D11, sw_format=BGRA -> DXGI_FORMAT_B8G8R8A8_UNORM
    AVBufferRef* hw_frm_ctx = av_hwframe_ctx_alloc(hw_dev_ctx);
    if (!hw_frm_ctx) {
        av_buffer_unref(&hw_dev_ctx);
        if (*out_d3d_ctx) { (*out_d3d_ctx)->Release(); *out_d3d_ctx = nullptr; }
        return false;
    }

    auto* frm_ctx          = reinterpret_cast<AVHWFramesContext*>(hw_frm_ctx->data);
    frm_ctx->format         = AV_PIX_FMT_D3D11;
    // Set sw_format to match the sender's actual format.
    // sw_format corresponds to the pool texture's DXGI format:
    //   AV_PIX_FMT_BGRA -> DXGI_FORMAT_B8G8R8A8_UNORM
    //   AV_PIX_FMT_RGBA -> DXGI_FORMAT_R8G8B8A8_UNORM
    // CopySubresourceRegion fails silently (no return value or error) on a format
    // mismatch, so the pool must match the sender format for correct output.
    // If FFmpeg/NVENC does not support AV_PIX_FMT_RGBA, av_hwframe_ctx_init() fails
    // and the caller falls back to the CPU path.
    frm_ctx->sw_format      = sw_fmt;
    frm_ctx->width          = static_cast<int>(width);
    frm_ctx->height         = static_cast<int>(height);
    frm_ctx->initial_pool_size = 4;  // pool for round-robin

    if (av_hwframe_ctx_init(hw_frm_ctx) < 0) {
        av_buffer_unref(&hw_frm_ctx);
        av_buffer_unref(&hw_dev_ctx);
        if (*out_d3d_ctx) { (*out_d3d_ctx)->Release(); *out_d3d_ctx = nullptr; }
        return false;
    }

    // 3. Set the HW frame context on the codec context
    ctx->hw_frames_ctx = av_buffer_ref(hw_frm_ctx);
    ctx->pix_fmt       = AV_PIX_FMT_D3D11;

    *out_hw_device_ctx = hw_dev_ctx;
    *out_hw_frames_ctx = hw_frm_ctx;
    return true;
}

bool EncoderController::init(const EncoderConfig& config,
                              uint32_t width, uint32_t height,
                              std::string& error,
                              void* d3d_device,
                              uint32_t sender_dxgi_format) {
    reset();
    config_ = config;
    width_  = width;
    height_ = height;

    // Try codecs in order: primary, then fallback
    std::vector<std::string> codecs_to_try = { config.codec };
    if (!config.fallback_codec.empty())
        codecs_to_try.push_back(config.fallback_codec);

    for (const auto& codec_name : codecs_to_try) {
        const AVCodec* codec = avcodec_find_encoder_by_name(codec_name.c_str());
        if (!codec) continue;

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) continue;

        ctx->codec_id     = codec->id;
        ctx->bit_rate     = static_cast<int64_t>(config.bitrate_kbps) * 1000;
        ctx->rc_max_rate  = ctx->bit_rate;
        ctx->rc_buffer_size = static_cast<int>(ctx->bit_rate);
        ctx->width        = static_cast<int>(width);
        ctx->height       = static_cast<int>(height);
        ctx->time_base    = {1, config.fps};
        ctx->framerate    = {config.fps, 1};
        ctx->gop_size     = config.gop_size;
        ctx->max_b_frames = config.max_b_frames;
        ctx->pix_fmt      = pick_pix_fmt(codec);
        if (config.threads > 0) ctx->thread_count = config.threads;

        // Spout input always comes from a PC monitor (sRGB, full range), so use
        // BT.709 regardless of resolution.
        // Declare color_range as JPEG (full range, 0-255) and tell sws to output
        // full range too, to minimize precision loss.
        ctx->color_range     = AVCOL_RANGE_JPEG;
        ctx->colorspace      = AVCOL_SPC_BT709;
        ctx->color_primaries = AVCOL_PRI_BT709;
        ctx->color_trc       = AVCOL_TRC_BT709;

        bool is_hw_codec = (codec_name.find("nvenc") != std::string::npos ||
                            codec_name.find("amf")   != std::string::npos ||
                            codec_name.find("qsv")   != std::string::npos);

        // Try the GPU zero-copy path only for a HW codec with a D3D11 device available
        bool gpu_init_ok = false;
        AVBufferRef* hw_device_ctx_tmp = nullptr;
        AVBufferRef* hw_frames_ctx_tmp = nullptr;
        ID3D11DeviceContext* d3d_ctx_tmp = nullptr;

        if (is_hw_codec && d3d_device) {
            // Choose the GPU pool's sw_format to match the sender's DXGI format.
            // CopySubresourceRegion fails silently on a format mismatch, so if the
            // sender is DXGI_FORMAT_R8G8B8A8_UNORM (RGBA), try AV_PIX_FMT_RGBA.
            // If D3D11VA does not support it, av_hwframe_ctx_init() fails and the block
            // below falls back to the CPU path.
            // 0 (unspecified) or DXGI_FORMAT_B8G8R8A8_UNORM uses AV_PIX_FMT_BGRA as before.
            AVPixelFormat gpu_sw_fmt = AV_PIX_FMT_BGRA;  // default: BGRA
            if (sender_dxgi_format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                gpu_sw_fmt = AV_PIX_FMT_RGBA;
            }

            gpu_init_ok = try_init_gpu_path(ctx, d3d_device,
                                            width, height,
                                            &hw_device_ctx_tmp,
                                            &hw_frames_ctx_tmp,
                                            &d3d_ctx_tmp,
                                            gpu_sw_fmt);
        }

        AVDictionary* opts = nullptr;

        if (!config.preset.empty())
            av_dict_set(&opts, "preset", config.preset.c_str(), 0);

        if (!is_hw_codec) {
            // libx264/libx265 accept tune and zerolatency
            if (!config.tune.empty())
                av_dict_set(&opts, "tune", config.tune.c_str(), 0);
        } else {
            // NVENC/AMF/QSV low-latency settings
            // tune=zerolatency can be incompatible with NVENC on the GPU path,
            // so it is skipped explicitly
            av_dict_set(&opts, "rc", "cbr", 0);
            av_dict_set(&opts, "delay", "0", 0);
        }

        int ret = avcodec_open2(ctx, codec, &opts);
        av_dict_free(&opts);

        if (ret < 0) {
            if (hw_frames_ctx_tmp) av_buffer_unref(&hw_frames_ctx_tmp);
            if (hw_device_ctx_tmp) av_buffer_unref(&hw_device_ctx_tmp);
            if (d3d_ctx_tmp)       { d3d_ctx_tmp->Release(); d3d_ctx_tmp = nullptr; }
            avcodec_free_context(&ctx);
            char buf[256];
            av_strerror(ret, buf, sizeof(buf));
            error = codec_name + ": avcodec_open2 failed: " + buf;
            continue; // try fallback
        }

        impl_->codec      = codec;
        impl_->codec_ctx  = ctx;
        impl_->codec_name = codec_name;

        if (gpu_init_ok) {
            // GPU zero-copy path established
            impl_->gpu_path       = true;
            impl_->hw_device_ctx  = hw_device_ctx_tmp;
            impl_->hw_frames_ctx  = hw_frames_ctx_tmp;
            impl_->d3d_ctx        = d3d_ctx_tmp;
            // Keep an AddRef'd pointer for device-lost detection.
            // Released in reset().
            auto* d3d11dev = static_cast<ID3D11Device*>(d3d_device);
            if (d3d11dev) {
                d3d11dev->AddRef();
                impl_->d3d_device = d3d11dev;
            }
            impl_->last_hw_frame  = av_frame_alloc();
            if (!impl_->last_hw_frame) {
                reset();
                error = "av_frame_alloc failed for last_hw_frame (GPU path)";
                return false;
            }

            impl_->pkt = av_packet_alloc();
            if (!impl_->pkt) {
                reset();
                error = "av_packet_alloc failed (GPU path)";
                return false;
            }
            impl_->frame_count = 0;
            return true;
        }

        // GPU init not needed or failed -> fall back to the CPU path
        // hw_frames_ctx was released inside try_init_gpu_path (on failure) or unref'd
        // above, so release ctx->hw_frames_ctx here if it remains.
        if (ctx->hw_frames_ctx) {
            av_buffer_unref(&ctx->hw_frames_ctx);
            ctx->hw_frames_ctx = nullptr;
        }
        ctx->pix_fmt = pick_pix_fmt(codec);

        // SwScale: RGBA → codec pixel format (SpoutDX outputs RGBA with bRGB=false)
        impl_->sws_ctx = sws_getContext(
            static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_RGBA,
            static_cast<int>(width), static_cast<int>(height), ctx->pix_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!impl_->sws_ctx) {
            avcodec_free_context(&impl_->codec_ctx);
            error = "sws_getContext failed";
            continue;
        }

        // Explicitly set the BT.709 matrix and full range.
        // Input RGBA is full range (JPEG); output YUV is also full range.
        {
            const int* in_coeff  = sws_getCoefficients(SWS_CS_DEFAULT);
            const int* out_coeff = sws_getCoefficients(SWS_CS_ITU709);
            // srcRange=1: full range input, dstRange=1: full range output
            int sws_ret = sws_setColorspaceDetails(impl_->sws_ctx,
                in_coeff,  1,
                out_coeff, 1,
                0, 1 << 16, 1 << 16);
            if (sws_ret < 0) {
                sws_freeContext(impl_->sws_ctx); impl_->sws_ctx = nullptr;
                avcodec_free_context(&impl_->codec_ctx);
                error = "sws_setColorspaceDetails failed";
                continue;
            }
        }

        impl_->yuv_frame = av_frame_alloc();
        if (!impl_->yuv_frame) {
            sws_freeContext(impl_->sws_ctx); impl_->sws_ctx = nullptr;
            avcodec_free_context(&impl_->codec_ctx);
            error = "av_frame_alloc failed";
            return false;
        }
        impl_->yuv_frame->format = ctx->pix_fmt;
        impl_->yuv_frame->width  = static_cast<int>(width);
        impl_->yuv_frame->height = static_cast<int>(height);
        ret = av_frame_get_buffer(impl_->yuv_frame, 32);
        if (ret < 0) {
            av_frame_free(&impl_->yuv_frame);
            sws_freeContext(impl_->sws_ctx); impl_->sws_ctx = nullptr;
            avcodec_free_context(&impl_->codec_ctx);
            error = "av_frame_get_buffer failed";
            return false;
        }

        impl_->pkt = av_packet_alloc();
        if (!impl_->pkt) {
            av_frame_free(&impl_->yuv_frame);
            sws_freeContext(impl_->sws_ctx); impl_->sws_ctx = nullptr;
            avcodec_free_context(&impl_->codec_ctx);
            error = "av_packet_alloc failed";
            return false;
        }

        impl_->frame_count = 0;
        return true;
    }

    if (error.empty())
        error = "No encoder found for '" + config.codec + "' or fallback '" + config.fallback_codec + "'";
    return false;
}

static bool drain_encoder(AVCodecContext* ctx, AVPacket* pkt,
                           std::vector<EncodedPacket>& out, int fps) {
    while (true) {
        int ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return false;

        EncodedPacket ep;
        ep.data.assign(pkt->data, pkt->data + pkt->size);
        ep.pts          = pkt->pts;
        ep.dts          = pkt->dts;
        ep.duration     = pkt->duration > 0 ? pkt->duration : 1;
        ep.is_key_frame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        ep.time_base_num = 1;
        ep.time_base_den = fps;
        out.push_back(std::move(ep));
        av_packet_unref(pkt);
    }
    return true;
}

bool EncoderController::encode(const FrameBuffer& frame,
                                const FrameMeta& /*meta*/,
                                std::vector<EncodedPacket>& out_packets,
                                bool content_changed) {
    if (!impl_->codec_ctx) return false;

    // --- GPU zero-copy path ---
    if (impl_->gpu_path) {
        // Check for device loss before CopySubresourceRegion;
        // bail out safely before an access violation can occur.
        if (impl_->d3d_device &&
            impl_->d3d_device->GetDeviceRemovedReason() != S_OK) {
            return false;
        }

        // Skip invalid frames with a null gpu_texture and no CPU data
        if (!frame.gpu_texture && frame.data.empty()) return true;

        bool need_copy = (content_changed || impl_->frame_count == 0 ||
                          !impl_->last_hw_frame ||
                          !impl_->last_hw_frame->buf[0]);

        if (need_copy) {
            AVFrame* hw_frame = av_frame_alloc();
            if (!hw_frame) return false;

            if (av_hwframe_get_buffer(impl_->hw_frames_ctx, hw_frame, 0) < 0) {
                av_frame_free(&hw_frame);
                return false;
            }

            auto* pool_tex   = reinterpret_cast<ID3D11Texture2D*>(hw_frame->data[0]);
            auto  pool_index = static_cast<UINT>(reinterpret_cast<intptr_t>(hw_frame->data[1]));

            if (frame.gpu_texture) {
                // GPU texture available: zero-copy transfer via CopySubresourceRegion
                auto* src_tex = static_cast<ID3D11Texture2D*>(frame.gpu_texture);
                impl_->d3d_ctx->CopySubresourceRegion(
                    pool_tex, pool_index, 0, 0, 0,
                    src_tex, 0, nullptr);
            } else {
                // CPU fallback: transfer CPU data via UpdateSubresource.
                //
                // Normally unreachable: with the GPU path active, SpoutMonitor runs in GPU mode
                // (set_gpu_mode(true)), so frame.data is empty, and a frame with empty data and
                // no gpu_texture returns early at the top of encode().
                // If reached, frame.data must match the pool's DXGI format; the CPU path
                // always yields RGBA, so a BGRA pool would swap R/B.
                D3D11_BOX box{};
                box.left   = 0;
                box.top    = 0;
                box.front  = 0;
                box.right  = frame.width;
                box.bottom = frame.height;
                box.back   = 1;
                UINT row_pitch = frame.width * 4;  // 4 bytes/pixel (RGBA/BGRA)
                if (row_pitch == 0) {
                    av_frame_free(&hw_frame);
                    return false;
                }
                impl_->d3d_ctx->UpdateSubresource(
                    pool_tex, pool_index, &box,
                    frame.data.data(), row_pitch, 0);
            }

            // Save to last_hw_frame for reuse when content_changed=false
            av_frame_unref(impl_->last_hw_frame);
            if (av_frame_ref(impl_->last_hw_frame, hw_frame) < 0) {
                av_frame_free(&hw_frame);
                return false;
            }
            av_frame_free(&hw_frame);
        }

        // Make a reference copy of last_hw_frame for encoding
        AVFrame* send_frame = av_frame_alloc();
        if (!send_frame) return false;
        if (av_frame_ref(send_frame, impl_->last_hw_frame) < 0) {
            av_frame_free(&send_frame);
            return false;
        }

        send_frame->pts        = impl_->frame_count++;
        send_frame->pict_type  = impl_->force_next_idr
            ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        impl_->force_next_idr  = false;

        int ret = avcodec_send_frame(impl_->codec_ctx, send_frame);
        av_frame_free(&send_frame);
        if (ret < 0) return false;

        return drain_encoder(impl_->codec_ctx, impl_->pkt, out_packets, config_.fps);
    }

    // --- CPU path (legacy behavior) ---
    if (!impl_->sws_ctx || !impl_->yuv_frame) return false;

    // Run RGBA->YUV conversion (sws_scale) only when the pixel content changed.
    //
    // Resending a static freeze frame encodes identical RGBA data at the configured
    // fps, so the conversion result is identical each time. sws_scale costs CPU
    // proportional to resolution, so reuse the previous YUV frame and still feed
    // the encoder with a new PTS.
    if (content_changed || impl_->frame_count == 0) {
        // Convert RGBA → codec pixel format (SpoutDX ReceiveImage with bRGB=false outputs RGBA)
        const uint8_t* in_data[1]  = { frame.data.data() };
        int            in_stride[1] = { static_cast<int>(frame.width) * 4 };

        int make_writable_ret = av_frame_make_writable(impl_->yuv_frame);
        if (make_writable_ret < 0) return false;
        sws_scale(impl_->sws_ctx,
                  in_data, in_stride, 0, static_cast<int>(frame.height),
                  impl_->yuv_frame->data, impl_->yuv_frame->linesize);
    }

    // PTS is a monotonically increasing frame counter.
    // FFmpeg's RTSP/RTP muxer truncates RTP timestamps to uint32_t naturally, so
    // 32-bit overflow is handled by the muxer.
    // Do not reset frame_count to 0: NVENC returns ENCODER_ENCODE_FAILED for
    // violating the monotonic-increase constraint.
    impl_->yuv_frame->pts = impl_->frame_count++;

    // If the force-IDR flag is set, request a keyframe and consume the flag,
    // so a full frame arrives right after an RTSP reconnect.
    impl_->yuv_frame->pict_type = impl_->force_next_idr
        ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    impl_->force_next_idr = false;

    int ret = avcodec_send_frame(impl_->codec_ctx, impl_->yuv_frame);
    if (ret < 0) return false;

    return drain_encoder(impl_->codec_ctx, impl_->pkt, out_packets, config_.fps);
}

bool EncoderController::flush(std::vector<EncodedPacket>& out_packets) {
    if (!impl_->codec_ctx) return true;
    int ret = avcodec_send_frame(impl_->codec_ctx, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return false;
    return drain_encoder(impl_->codec_ctx, impl_->pkt, out_packets, config_.fps);
}

void EncoderController::reset() {
    if (impl_->pkt)           { av_packet_free(&impl_->pkt); }
    if (impl_->yuv_frame)     { av_frame_free(&impl_->yuv_frame); }
    if (impl_->sws_ctx)       { sws_freeContext(impl_->sws_ctx); impl_->sws_ctx = nullptr; }
    if (impl_->last_hw_frame) { av_frame_free(&impl_->last_hw_frame); }
    if (impl_->hw_frames_ctx) { av_buffer_unref(&impl_->hw_frames_ctx); }
    if (impl_->hw_device_ctx) { av_buffer_unref(&impl_->hw_device_ctx); }
    if (impl_->d3d_ctx)       { impl_->d3d_ctx->Release(); impl_->d3d_ctx = nullptr; }
    if (impl_->d3d_device)    { impl_->d3d_device->Release(); impl_->d3d_device = nullptr; }
    if (impl_->codec_ctx)     { avcodec_free_context(&impl_->codec_ctx); }
    impl_->frame_count    = 0;
    impl_->force_next_idr = false;
    impl_->codec          = nullptr;
    impl_->codec_name.clear();
    impl_->gpu_path       = false;
    width_  = 0;
    height_ = 0;
}

void EncoderController::request_next_idr() {
    impl_->force_next_idr = true;
}

bool EncoderController::gpu_path_active() const {
    return impl_->gpu_path;
}

int EncoderController::fps() const {
    return config_.fps;
}

int EncoderController::bitrate_kbps() const {
    return config_.bitrate_kbps;
}

EncoderController::CodecInfo EncoderController::get_codec_info() const {
    CodecInfo ci;
    if (!impl_->codec_ctx) return ci;
    AVCodecContext* ctx = impl_->codec_ctx;
    ci.codec_id        = static_cast<int>(ctx->codec_id);
    ci.width           = ctx->width;
    ci.height          = ctx->height;
    ci.fps             = config_.fps;
    ci.bit_rate        = static_cast<int>(ctx->bit_rate);
    ci.time_base_num   = ctx->time_base.num;
    ci.time_base_den   = ctx->time_base.den;
    ci.color_range     = static_cast<int>(ctx->color_range);
    ci.colorspace      = static_cast<int>(ctx->colorspace);
    ci.color_primaries = static_cast<int>(ctx->color_primaries);
    ci.color_trc       = static_cast<int>(ctx->color_trc);
    if (ctx->extradata && ctx->extradata_size > 0) {
        ci.extradata.assign(ctx->extradata,
                            ctx->extradata + ctx->extradata_size);
    }
    return ci;
}
