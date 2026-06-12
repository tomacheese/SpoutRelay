#pragma once
#include "common/types.hpp"
#include "config/config_loader.hpp"
#include <string>

/// @brief Spout 映像/デバイスが見つからない間に配信する
///        プレースホルダ (NO SIGNAL) 映像を 1 枚描画する。
///
/// GDI を用いて指定解像度のフレームを生成する。背景色で塗りつぶし、
/// 中央にメッセージ（と任意で待機中の sender 名）を描画する。
///
/// エンコーダーは常に AV_PIX_FMT_RGBA 固定で入力を扱う
/// (encoder_controller.cpp の sws_getContext) ため、返却する FrameBuffer は
/// 必ず PixelFormat::RGBA・RGBA バイト順で構築する。
///
/// @param cfg         placeholder 設定 (メッセージ・色・sender 名表示の有無)
/// @param sender_name 待機中の Spout sender 名 (show_sender_name 時に表示)
/// @param width       出力解像度の幅 (px)
/// @param height      出力解像度の高さ (px)
/// @return width * height * 4 バイトの RGBA フレームバッファ
FrameBuffer render_placeholder_frame(const AppConfig::Placeholder& cfg,
                                      const std::string& sender_name,
                                      uint32_t width, uint32_t height);
