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

    bool             force_next_idr = false;  ///< 次フレームを IDR として強制出力するフラグ

    // --- GPU ゼロコピーパス ---
    bool             gpu_path     = false;
    AVBufferRef*     hw_device_ctx = nullptr;
    AVBufferRef*     hw_frames_ctx = nullptr;
    AVFrame*         last_hw_frame = nullptr; ///< content_changed=false 時に再利用する HW フレーム
    ID3D11DeviceContext* d3d_ctx   = nullptr; ///< CopySubresourceRegion 用（借用）
    ID3D11Device*    d3d_device    = nullptr; ///< デバイスロスト検出用（AddRef 保持）
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
// GPU ゼロコピーパス初期化ヘルパー
// 成功時 true を返し codec_ctx->hw_frames_ctx をセットする。
// 失敗時は確保済みリソースを解放して false を返す（CPU フォールバック用）。
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

    // 1. FFmpeg HW デバイスコンテキストを既存の D3D11 デバイスでラップする
    AVBufferRef* hw_dev_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_dev_ctx) return false;

    auto* dev_ctx  = reinterpret_cast<AVHWDeviceContext*>(hw_dev_ctx->data);
    auto* d3d11ctx = static_cast<AVD3D11VADeviceContext*>(dev_ctx->hwctx);

    d3d11dev->AddRef();               // FFmpeg が Release するため
    d3d11ctx->device = d3d11dev;

    // D3D11 デバイスコンテキストを取得して保存（CopySubresourceRegion 用）
    d3d11dev->GetImmediateContext(out_d3d_ctx);

    if (av_hwdevice_ctx_init(hw_dev_ctx) < 0) {
        av_buffer_unref(&hw_dev_ctx);
        if (*out_d3d_ctx) { (*out_d3d_ctx)->Release(); *out_d3d_ctx = nullptr; }
        return false;
    }

    // 2. HW フレームコンテキストを構築する
    //    format=D3D11, sw_format=BGRA → DXGI_FORMAT_B8G8R8A8_UNORM
    AVBufferRef* hw_frm_ctx = av_hwframe_ctx_alloc(hw_dev_ctx);
    if (!hw_frm_ctx) {
        av_buffer_unref(&hw_dev_ctx);
        if (*out_d3d_ctx) { (*out_d3d_ctx)->Release(); *out_d3d_ctx = nullptr; }
        return false;
    }

    auto* frm_ctx          = reinterpret_cast<AVHWFramesContext*>(hw_frm_ctx->data);
    frm_ctx->format         = AV_PIX_FMT_D3D11;
    // センダーの実フォーマットに合わせて sw_format を設定する。
    // sw_format はプールテクスチャの DXGI フォーマットと対応する:
    //   AV_PIX_FMT_BGRA → DXGI_FORMAT_B8G8R8A8_UNORM (87)
    //   AV_PIX_FMT_RGBA → DXGI_FORMAT_R8G8B8A8_UNORM (28)
    // CopySubresourceRegion はフォーマット不一致時に戻り値もエラーも返さず無音で失敗するため、
    // センダーフォーマットと一致したプールを用意することが正しい映像出力の必要条件。
    // AV_PIX_FMT_RGBA が FFmpeg/NVENC でサポートされない場合は av_hwframe_ctx_init() が失敗し、
    // 呼び出し元が CPU パスへフォールバックする。
    frm_ctx->sw_format      = sw_fmt;
    frm_ctx->width          = static_cast<int>(width);
    frm_ctx->height         = static_cast<int>(height);
    frm_ctx->initial_pool_size = 4;  // ラウンドロビン用プール

    if (av_hwframe_ctx_init(hw_frm_ctx) < 0) {
        av_buffer_unref(&hw_frm_ctx);
        av_buffer_unref(&hw_dev_ctx);
        if (*out_d3d_ctx) { (*out_d3d_ctx)->Release(); *out_d3d_ctx = nullptr; }
        return false;
    }

    // 3. コーデックコンテキストに HW フレームコンテキストを設定する
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
        ctx->width        = static_cast<int>(width);
        ctx->height       = static_cast<int>(height);
        ctx->time_base    = {1, config.fps};
        ctx->framerate    = {config.fps, 1};
        ctx->gop_size     = config.gop_size;
        ctx->max_b_frames = config.max_b_frames;
        ctx->pix_fmt      = pick_pix_fmt(codec);
        if (config.threads > 0) ctx->thread_count = config.threads;

        // Spout 入力は常に PC モニター由来の sRGB (full range) なので、
        // 解像度によらず BT.709 で統一する。
        // color_range は JPEG (full range, 0-255) として宣言し、
        // sws にも full range 出力を指示することで精度損失を最小化する。
        ctx->color_range     = AVCOL_RANGE_JPEG;
        ctx->colorspace      = AVCOL_SPC_BT709;
        ctx->color_primaries = AVCOL_PRI_BT709;
        ctx->color_trc       = AVCOL_TRC_BT709;

        bool is_hw_codec = (codec_name.find("nvenc") != std::string::npos ||
                            codec_name.find("amf")   != std::string::npos ||
                            codec_name.find("qsv")   != std::string::npos);

        // GPU ゼロコピーパスの試行: HW コーデック かつ D3D11 デバイスが利用可能な場合のみ
        bool gpu_init_ok = false;
        AVBufferRef* hw_device_ctx_tmp = nullptr;
        AVBufferRef* hw_frames_ctx_tmp = nullptr;
        ID3D11DeviceContext* d3d_ctx_tmp = nullptr;

        if (is_hw_codec && d3d_device) {
            // センダーの DXGI フォーマットに合わせて GPU プールの sw_format を選択する。
            // CopySubresourceRegion はフォーマット不一致時に戻り値もエラーも返さず無音で失敗するため、
            // センダーが RGBA (28=DXGI_FORMAT_R8G8B8A8_UNORM) なら AV_PIX_FMT_RGBA を試みる。
            // AV_PIX_FMT_RGBA が FFmpeg D3D11VA で未サポートの場合は av_hwframe_ctx_init() が
            // 失敗し、以下のブロックで CPU パスへ自動フォールバックする。
            // 0 (未指定) または 87 (B8G8R8A8_UNORM) は従来通り AV_PIX_FMT_BGRA を使用する。
            AVPixelFormat gpu_sw_fmt = AV_PIX_FMT_BGRA;  // デフォルト: BGRA
            if (sender_dxgi_format == 28) {               // DXGI_FORMAT_R8G8B8A8_UNORM
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
            // NVENC/AMF/QSV 低遅延設定
            // GPU パスでは tune=zerolatency が NVENC と非互換になるケースがあるため
            // 明示的にスキップする（issue #14 制約）
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
            // GPU ゼロコピーパス確立
            impl_->gpu_path       = true;
            impl_->hw_device_ctx  = hw_device_ctx_tmp;
            impl_->hw_frames_ctx  = hw_frames_ctx_tmp;
            impl_->d3d_ctx        = d3d_ctx_tmp;
            // デバイスロスト検出用にポインタを AddRef して保持する。
            // reset() で Release する。
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

        // GPU 初期化不要 or 失敗 → CPU パスにフォールバック
        // hw_frames_ctx は try_init_gpu_path 内で解放済み（失敗時）か
        // 上で unref 済みなので、ここでは ctx->hw_frames_ctx が残っていれば解放する。
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

        // BT.709 変換行列と full range を明示的に設定する
        // 入力 RGBA は full range (JPEG)、出力 YUV も full range で統一する
        {
            const int* in_coeff  = sws_getCoefficients(SWS_CS_DEFAULT);
            const int* out_coeff = sws_getCoefficients(SWS_CS_ITU709);
            // srcRange=1: full range 入力、dstRange=1: full range 出力
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

    // --- GPU ゼロコピーパス ---
    if (impl_->gpu_path) {
        // CopySubresourceRegion の前にデバイスロストを確認する。
        // デバイスロスト時はアクセス違反が発生する前に安全に脱出する。
        if (impl_->d3d_device &&
            impl_->d3d_device->GetDeviceRemovedReason() != S_OK) {
            return false;
        }

        // gpu_texture が null だが CPU データもない不正フレームはスキップする
        if (!frame.gpu_texture && frame.data.empty()) return true;

        bool need_copy = (content_changed || impl_->frame_count == 0 ||
                          !impl_->last_hw_frame ||
                          !impl_->last_hw_frame->buf[0]);

        if (need_copy) {
            // プールから新規 HW フレームを取得する
            AVFrame* hw_frame = av_frame_alloc();
            if (!hw_frame) return false;

            if (av_hwframe_get_buffer(impl_->hw_frames_ctx, hw_frame, 0) < 0) {
                av_frame_free(&hw_frame);
                return false;
            }

            // D3D11 プールテクスチャを取り出す
            auto* pool_tex   = reinterpret_cast<ID3D11Texture2D*>(hw_frame->data[0]);
            auto  pool_index = static_cast<UINT>(reinterpret_cast<intptr_t>(hw_frame->data[1]));

            if (frame.gpu_texture) {
                // GPU テクスチャが利用可能: CopySubresourceRegion でゼロコピー転送
                auto* src_tex = static_cast<ID3D11Texture2D*>(frame.gpu_texture);
                impl_->d3d_ctx->CopySubresourceRegion(
                    pool_tex, pool_index, 0, 0, 0,
                    src_tex, 0, nullptr);
            } else {
                // CPU フォールバック: UpdateSubresource で CPU データを転送する。
                //
                // 通常、GPU ゼロコピーパスが有効な場合 SpoutMonitor::receive_latest_frame()
                // は GPU モード (set_gpu_mode(true)) で動作するため、buf.data.clear() により
                // frame.data は常に空になる。frame.data が空かつ gpu_texture が null の場合は
                // encode() 冒頭の早期 return (frame.data.empty() && !frame.gpu_texture) で
                // スキップされるため、このブランチは通常到達しない。
                //
                // このブランチに到達するとしたら SpoutMonitor が CPU モードのまま encode()
                // に渡された場合だが、その場合 gpu_path_active() == true であれば
                // frame.data の内容がプールの DXGI フォーマットと一致している必要がある。
                // SpoutMonitor CPU パスは常に RGBA を返すため、プールが BGRA の場合は
                // R/B が入れ替わった色化けが発生しうる。GPU モードの切り替えは
                // supervisor.cpp で gpu_path_active() 確認後に行われているため、
                // 正常なコードパスでは本ブランチでの不一致は発生しない。
                D3D11_BOX box{};
                box.left   = 0;
                box.top    = 0;
                box.front  = 0;
                box.right  = frame.width;
                box.bottom = frame.height;
                box.back   = 1;
                UINT row_pitch = frame.width * 4;  // 4 bytes/pixel (RGBA/BGRA 共通)
                if (row_pitch == 0) {
                    av_frame_free(&hw_frame);
                    return false;
                }
                impl_->d3d_ctx->UpdateSubresource(
                    pool_tex, pool_index, &box,
                    frame.data.data(), row_pitch, 0);
            }

            // last_hw_frame に保存して content_changed=false 時に再利用する
            av_frame_unref(impl_->last_hw_frame);
            if (av_frame_ref(impl_->last_hw_frame, hw_frame) < 0) {
                av_frame_free(&hw_frame);
                return false;
            }
            av_frame_free(&hw_frame);
        }

        // エンコード用に last_hw_frame の参照コピーを作成する
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

    // --- CPU パス（従来動作）---
    if (!impl_->sws_ctx || !impl_->yuv_frame) return false;

    // ピクセル内容が変化している場合のみ RGBA→YUV 変換 (sws_scale) を実行する。
    //
    // 静止画面のフリーズフレーム再送時は同一の RGBA データを設定 fps で繰り返し
    // エンコードするが、変換結果も毎回同一になる。sws_scale は解像度に比例した
    // CPU コストがかかるため、直前の変換済み YUV フレームをそのまま再利用する
    // ことで変換コストを排除しつつエンコーダーへは新しい PTS でフレームを送り続ける。
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

    // PTS は単調増加フレームカウンタ。
    // FFmpeg の RTSP/RTP muxer は内部で RTP タイムスタンプを uint32_t に
    // 自然切り捨てするため、32-bit オーバーフローは muxer 側で自動処理される。
    // 手動で frame_count を 0 にリセットすると NVENC が単調増加制約違反で
    // ENCODER_ENCODE_FAILED を返すため、リセットは行わない。
    impl_->yuv_frame->pts = impl_->frame_count++;

    // IDR 強制フラグが立っている場合はキーフレームを要求し、フラグを消費する。
    // RTSP 再接続後に即座に完全フレームが届くようにするため。
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
