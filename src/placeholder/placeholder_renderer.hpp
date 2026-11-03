#pragma once
#include "common/types.hpp"
#include "config/config_loader.hpp"
#include <string>

/// @brief Renders one placeholder (NO SIGNAL) frame, streamed while no Spout
///        source/device is found.
///
/// Generates a frame at the given resolution using GDI: fills the background and
/// draws the message at the center (optionally with the awaited sender name).
///
/// The encoder always takes AV_PIX_FMT_RGBA input (sws_getContext in
/// encoder_controller.cpp), so the returned FrameBuffer is always built as
/// PixelFormat::RGBA in RGBA byte order.
///
/// @param cfg         placeholder settings (message, colors, whether to show sender name)
/// @param sender_name awaited Spout sender name (shown when show_sender_name is set)
/// @param width       output width (px)
/// @param height      output height (px)
/// @return RGBA frame buffer of width * height * 4 bytes
FrameBuffer render_placeholder_frame(const AppConfig::Placeholder& cfg,
                                      const std::string& sender_name,
                                      uint32_t width, uint32_t height);
